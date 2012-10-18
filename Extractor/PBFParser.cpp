
#include "PBFParser.h"


PBFParser::PBFParser(const char * fileName) : myLuaState(NULL) {
    GOOGLE_PROTOBUF_VERIFY_VERSION;
    //TODO: What is the bottleneck here? Filling the queue or reading the stuff from disk?
    threadDataQueue = boost::make_shared<ConcurrentQueue<_ThreadData*> >( 2500 ); /* Max 2500 items in queue, hardcoded. */
    input.open(fileName, std::ios::in | std::ios::binary);

    if (!input) {
        std::cerr << fileName << ": File not found." << std::endl;
    }

    blockCount = 0;
    groupCount = 0;

    //Dummy initialization
    wayCallback = NULL;
    nodeCallback = NULL;
    restrictionCallback = NULL;
}

PBFParser::~PBFParser() {
    if(input.is_open())
        input.close();

    // Clean up any leftover ThreadData objects in the queue
    _ThreadData* td;
    while (threadDataQueue->try_pop(td)) {
        delete td;
    }
    google::protobuf::ShutdownProtobufLibrary();

#ifndef NDEBUG
    std::cout << "[info] blocks: " << blockCount << std::endl;
    std::cout << "[info] groups: " << groupCount << std::endl;
#endif
}

PBFParser::Endianness PBFParser::getMachineEndianness() const {
    int i(1);
    char *p = (char *) &i;
    if (p[0] == 1)
        return LittleEndian;
    return BigEndian;
}
   
bool PBFParser::readPBFBlobHeader(std::fstream& stream, _ThreadData * threadData) {
    int size(0);
    stream.read((char *)&size, sizeof(int));
    size = swapEndian(size);
    if(stream.eof()) {
        return false;
    }
    if ( size > MAX_BLOB_HEADER_SIZE || size < 0 ) {
        return false;
    }
    char *data = new char[size];
    stream.read(data, size*sizeof(data[0]));

    if ( !(threadData->PBFBlobHeader).ParseFromArray( data, size ) ){
        delete[] data;
        return false;
    }
    delete[] data;
    return true;
}
 
bool PBFParser::RegisterCallbacks(bool (*nodeCallbackPointer)(_Node), bool (*restrictionCallbackPointer)(_RawRestrictionContainer), bool (*wayCallbackPointer)(_Way) ) {
    nodeCallback = *nodeCallbackPointer;
    wayCallback = *wayCallbackPointer;
    restrictionCallback = *restrictionCallbackPointer;
    return true;
}

void PBFParser::RegisterLUAState(lua_State *ml) {
    myLuaState = ml;
}


bool PBFParser::Init() {
    _ThreadData initData;
    /** read Header */
    if(!readPBFBlobHeader(input, &initData)) {
        return false;
    }

    if(readBlob(input, &initData)) {
        if(!initData.PBFHeaderBlock.ParseFromArray(&(initData.charBuffer[0]), initData.charBuffer.size() ) ) {
            std::cerr << "[error] Header not parseable!" << std::endl;
            return false;
        }

        for(int i = 0; i < initData.PBFHeaderBlock.required_features_size(); ++i) {
            const std::string& feature = initData.PBFHeaderBlock.required_features( i );
            bool supported = false;
            if ( feature == "OsmSchema-V0.6" )
                supported = true;
            else if ( feature == "DenseNodes" )
                supported = true;

            if ( !supported ) {
                std::cerr << "[error] required feature not supported: " << feature.data() << std::endl;
                return false;
            }
        }
    } else {
        std::cerr << "[error] blob not loaded!" << std::endl;
    }
    return true;
}

void PBFParser::ReadData() {
    bool keepRunning = true;
    do {
        _ThreadData *threadData = new _ThreadData();
        keepRunning = readNextBlock(input, threadData);

        if (keepRunning)
            threadDataQueue->push(threadData);
        else {
            threadDataQueue->push(NULL); // No more data to read, parse stops when NULL encountered
            delete threadData;
        }
    } while(keepRunning);
}

void PBFParser::ParseData() {
    while (1) {
        _ThreadData *threadData;
        threadDataQueue->wait_and_pop(threadData);
        if (threadData == NULL) {
            cout << "Parse Data Thread Finished" << endl;
            threadDataQueue->push(NULL); // Signal end of data for other threads
            break;
        }

        loadBlock(threadData);

        for(int i = 0; i < threadData->PBFprimitiveBlock.primitivegroup_size(); i++) {
            threadData->currentGroupID = i;
            loadGroup(threadData);

            if(threadData->entityTypeIndicator == TypeNode)
                parseNode(threadData);
            if(threadData->entityTypeIndicator == TypeWay)
                parseWay(threadData);
            if(threadData->entityTypeIndicator == TypeRelation)
                parseRelation(threadData);
            if(threadData->entityTypeIndicator == TypeDenseNode)
                parseDenseNode(threadData);
        }

        delete threadData;
        threadData = NULL;
    }
}

bool PBFParser::Parse() {
    // Start the read and parse threads
    boost::thread readThread(boost::bind(&PBFParser::ReadData, this));

    //Open several parse threads that are synchronized before call to
    boost::thread parseThread(boost::bind(&PBFParser::ParseData, this));

    // Wait for the threads to finish
    readThread.join();
    parseThread.join();

    return true;
}

void PBFParser::parseDenseNode(_ThreadData * threadData) {
    const OSMPBF::DenseNodes& dense = threadData->PBFprimitiveBlock.primitivegroup( threadData->currentGroupID ).dense();
    int denseTagIndex = 0;
    int m_lastDenseID = 0;
    int m_lastDenseLatitude = 0;
    int m_lastDenseLongitude = 0;

    for(int i = 0; i < dense.id_size(); i++) {

        m_lastDenseID += dense.id( i );
        m_lastDenseLatitude += dense.lat( i );
        m_lastDenseLongitude += dense.lon( i );
        ImportNode n;
        n.id = m_lastDenseID;
        n.lat = 100000*( ( double ) m_lastDenseLatitude * threadData->PBFprimitiveBlock.granularity() +threadData-> PBFprimitiveBlock.lat_offset() ) / NANO;
        n.lon = 100000*( ( double ) m_lastDenseLongitude * threadData->PBFprimitiveBlock.granularity() + threadData->PBFprimitiveBlock.lon_offset() ) / NANO;
        while (denseTagIndex < dense.keys_vals_size()) {
            int tagValue = dense.keys_vals( denseTagIndex );
            if(tagValue == 0) {
                denseTagIndex++;
                break;
            }
            int keyValue = dense.keys_vals ( denseTagIndex+1 );
            std::string key = threadData->PBFprimitiveBlock.stringtable().s(tagValue).data();
            std::string value = threadData->PBFprimitiveBlock.stringtable().s(keyValue).data();
            n.keyVals.Add(key, value);
            denseTagIndex += 2;
        }

        /** Pass the unpacked node to the LUA call back **/
        try {
            luabind::call_function<int>(
                    myLuaState,
                    "node_function",
                    boost::ref(n)
            );
            if(!(*nodeCallback)(n))
                std::cerr << "[PBFParser] dense node not parsed" << std::endl;
        } catch (const luabind::error &er) {
            cerr << er.what() << endl;
            lua_State* Ler=er.state();
            report_errors(Ler, -1);
        }
        catch (...) {
            ERR("Unknown error occurred during PBF dense node parsing!");
        }
    }
}

void PBFParser::parseNode(_ThreadData * ) {
    ERR("Parsing of simple nodes not supported. PBF should use dense nodes");
//        _Node n;
//        if(!(*nodeCallback)(n))
//            std::cerr << "[PBFParser] simple node not parsed" << std::endl;
}

void PBFParser::parseRelation(_ThreadData * threadData) {
    const OSMPBF::PrimitiveGroup& group = threadData->PBFprimitiveBlock.primitivegroup( threadData->currentGroupID );
    for(int i = 0; i < group.relations_size(); i++ ) {
        const OSMPBF::Relation& inputRelation = threadData->PBFprimitiveBlock.primitivegroup( threadData->currentGroupID ).relations(i);
        bool isRestriction = false;
        bool isOnlyRestriction = false;
        for(int k = 0; k < inputRelation.keys_size(); k++) {
            const std::string key = threadData->PBFprimitiveBlock.stringtable().s(inputRelation.keys(k));
            const std::string val = threadData->PBFprimitiveBlock.stringtable().s(inputRelation.vals(k));
            if ("type" == key && "restriction" == val) {
                isRestriction = true;
            }
            if ("restriction" == key) {
                if(val.find("only_") == 0)
                    isOnlyRestriction = true;
            }

        }
        if(isRestriction) {
            long long lastRef = 0;
            _RawRestrictionContainer currentRestrictionContainer(isOnlyRestriction);
            for(int rolesIndex = 0; rolesIndex < inputRelation.roles_sid_size(); rolesIndex++) {
                string role(threadData->PBFprimitiveBlock.stringtable().s( inputRelation.roles_sid( rolesIndex ) ).data());
                lastRef += inputRelation.memids(rolesIndex);

                if(false == ("from" == role || "to" == role || "via" == role)) {
                    continue;
                }

                switch(inputRelation.types(rolesIndex)) {
                case 0: //node
                    if("from" == role || "to" == role) //Only via should be a node
                        continue;
                    assert("via" == role);
                    if(UINT_MAX != currentRestrictionContainer.viaNode)
                        currentRestrictionContainer.viaNode = UINT_MAX;
                    assert(UINT_MAX == currentRestrictionContainer.viaNode);
                    currentRestrictionContainer.restriction.viaNode = lastRef;
                    break;
                case 1: //way
                    assert("from" == role || "to" == role || "via" == role);
                    if("from" == role) {
                        currentRestrictionContainer.fromWay = lastRef;
                    }
                    if ("to" == role) {
                        currentRestrictionContainer.toWay = lastRef;
                    }
                    if ("via" == role) {
                        assert(currentRestrictionContainer.restriction.toNode == UINT_MAX);
                        currentRestrictionContainer.viaNode = lastRef;
                    }
                    break;
                case 2: //relation, not used. relations relating to relations are evil.
                    continue;
                    assert(false);
                    break;

                default: //should not happen
                    cout << "unknown";
                    assert(false);
                    break;
                }
            }
            //                if(UINT_MAX != currentRestriction.viaNode) {
            //                    cout << "restr from " << currentRestriction.from << " via ";
            //                    cout << "node " << currentRestriction.viaNode;
            //                    cout << " to " << currentRestriction.to << endl;
            //                }
            if(!(*restrictionCallback)(currentRestrictionContainer))
                std::cerr << "[PBFParser] relation not parsed" << std::endl;
        }
    }
}

void PBFParser::parseWay(_ThreadData * threadData) {
    if( threadData->PBFprimitiveBlock.primitivegroup( threadData->currentGroupID ).ways_size() > 0) {
        for(int i = 0; i < threadData->PBFprimitiveBlock.primitivegroup( threadData->currentGroupID ).ways_size(); i++) {
            const OSMPBF::Way& inputWay = threadData->PBFprimitiveBlock.primitivegroup( threadData->currentGroupID ).ways( i );
            _Way w;
            w.id = inputWay.id();
            unsigned pathNode(0);
            for(int i = 0; i < inputWay.refs_size(); ++i) {
                pathNode += inputWay.refs(i);
                w.path.push_back(pathNode);
            }
            assert(inputWay.keys_size() == inputWay.vals_size());
            for(int i = 0; i < inputWay.keys_size(); ++i) {
                const std::string key = threadData->PBFprimitiveBlock.stringtable().s(inputWay.keys(i));
                const std::string val = threadData->PBFprimitiveBlock.stringtable().s(inputWay.vals(i));
                w.keyVals.Add(key, val);
            }

            /** Pass the unpacked way to the LUA call back **/
            try {
                luabind::call_function<int>(
                    myLuaState,
                    "way_function",
                    boost::ref(w),
                    w.path.size()
                );
                if(!(*wayCallback)(w)) {
                    std::cerr << "[PBFParser] way not parsed" << std::endl;
                }
            } catch (const luabind::error &er) {
                cerr << er.what() << endl;
                lua_State* Ler=er.state();
                report_errors(Ler, -1);
            }
            catch (...) {
                cerr<<"Unknown error!"<<endl;
            }
        }
    }
}

void PBFParser::loadGroup(_ThreadData * threadData) {
    groupCount++;

    const OSMPBF::PrimitiveGroup& group = threadData->PBFprimitiveBlock.primitivegroup( threadData->currentGroupID );
    threadData->entityTypeIndicator = 0;
    if ( group.nodes_size() != 0 ) {
        threadData->entityTypeIndicator = TypeNode;
    }
    if ( group.ways_size() != 0 ) {
        threadData->entityTypeIndicator = TypeWay;
    }
    if ( group.relations_size() != 0 ) {
        threadData->entityTypeIndicator = TypeRelation;
    }
    if ( group.has_dense() )  {
        threadData->entityTypeIndicator = TypeDenseNode;
        assert( group.dense().id_size() != 0 );
    }
    assert( threadData->entityTypeIndicator != 0 );
}

void PBFParser::loadBlock(_ThreadData * threadData) {
    ++blockCount;
    threadData->currentGroupID = 0;
    threadData->currentEntityID = 0;
}


bool PBFParser::unpackZLIB(std::fstream &, _ThreadData * threadData) {
    unsigned rawSize = threadData->PBFBlob.raw_size();
    char* unpackedDataArray = (char*)malloc(rawSize);
    z_stream compressedDataStream;
    compressedDataStream.next_in = ( unsigned char* ) threadData->PBFBlob.zlib_data().data();
    compressedDataStream.avail_in = threadData->PBFBlob.zlib_data().size();
    compressedDataStream.next_out = ( unsigned char* ) unpackedDataArray;
    compressedDataStream.avail_out = rawSize;
    compressedDataStream.zalloc = Z_NULL;
    compressedDataStream.zfree = Z_NULL;
    compressedDataStream.opaque = Z_NULL;
    int ret = inflateInit( &compressedDataStream );
    if ( ret != Z_OK ) {
        std::cerr << "[error] failed to init zlib stream" << std::endl;
        free(unpackedDataArray);
        return false;
    }

    ret = inflate( &compressedDataStream, Z_FINISH );
    if ( ret != Z_STREAM_END ) {
        std::cerr << "[error] failed to inflate zlib stream" << std::endl;
        std::cerr << "[error] Error type: " << ret << std::endl;
        free(unpackedDataArray);
        return false;
    }

    ret = inflateEnd( &compressedDataStream );
    if ( ret != Z_OK ) {
        std::cerr << "[error] failed to deinit zlib stream" << std::endl;
        free(unpackedDataArray);
        return false;
    }

    threadData->charBuffer.clear(); threadData->charBuffer.resize(rawSize);
    for(unsigned i = 0; i < rawSize; i++) {
        threadData->charBuffer[i] = unpackedDataArray[i];
    }
    free(unpackedDataArray);
    return true;
}

bool PBFParser::unpackLZMA(std::fstream &, _ThreadData * ) {
    return false;
}

bool PBFParser::readBlob(std::fstream& stream, _ThreadData * threadData) {
    if(stream.eof())
        return false;

    int size = threadData->PBFBlobHeader.datasize();
    if ( size < 0 || size > MAX_BLOB_SIZE ) {
        std::cerr << "[error] invalid Blob size:" << size << std::endl;
        return false;
    }

    char* data = (char*)malloc(size);
    stream.read(data, sizeof(data[0])*size);

    if ( !threadData->PBFBlob.ParseFromArray( data, size ) ) {
        std::cerr << "[error] failed to parse blob" << std::endl;
        free(data);
        return false;
    }

    if ( threadData->PBFBlob.has_raw() ) {
        const std::string& data = threadData->PBFBlob.raw();
        threadData->charBuffer.clear();
        threadData->charBuffer.resize( data.size() );
        for ( unsigned i = 0; i < data.size(); i++ ) {
            threadData->charBuffer[i] = data[i];
        }
    } else if ( threadData->PBFBlob.has_zlib_data() ) {
        if ( !unpackZLIB(stream, threadData) ) {
            std::cerr << "[error] zlib data encountered that could not be unpacked" << std::endl;
            free(data);
            return false;
        }
    } else if ( threadData->PBFBlob.has_lzma_data() ) {
        if ( !unpackLZMA(stream, threadData) )
            std::cerr << "[error] lzma data encountered that could not be unpacked" << std::endl;
        free(data);
        return false;
    } else {
        std::cerr << "[error] Blob contains no data" << std::endl;
        free(data);
        return false;
    }
    free(data);
    return true;
}

bool PBFParser::readNextBlock(std::fstream& stream, _ThreadData * threadData) {
    if(stream.eof()) {
        return false;
    }

    if ( !readPBFBlobHeader(stream, threadData) ){
        return false;
    }

    if ( threadData->PBFBlobHeader.type() != "OSMData" ) {
        return false;
    }

    if ( !readBlob(stream, threadData) ) {
        return false;
    }

    if ( !threadData->PBFprimitiveBlock.ParseFromArray( &(threadData->charBuffer[0]), threadData-> charBuffer.size() ) ) {
        ERR("failed to parse PrimitiveBlock");
        return false;
    }
    return true;
}
