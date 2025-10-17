#include <Compression/ICompressionCodec.h>
#include <Compression/CompressionInfo.h>
#include <Compression/CompressionFactory.h>
#include <base/unaligned.h>
#include <Parsers/IAST.h>
#include <Parsers/ASTLiteral.h>
#include <Parsers/ASTFunction.h>
#include <Poco/Logger.h>
#include <Common/logger_useful.h>

#include <openzl/zl_compress.h>
#include <openzl/zl_compressor.h>
#include <openzl/zl_decompress.h>
#include <openzl/zl_public_nodes.h>
#include <openzl/zl_selector.h>

#pragma clang diagnostic ignored "-Wused-but-marked-unused"
#pragma clang diagnostic ignored "-Wunused-value"
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wc99-extensions"


namespace DB
{

ZL_GraphID buildCompressor(ZL_Compressor* compressor, String mode, UInt8 width);
void parameterizeCompressor(ZL_Compressor* compressor, int level);

class CompressionCodecOpenZL : public ICompressionCodec
{
public:
    explicit CompressionCodecOpenZL(String mode_, UInt8 numeric_bytes_size_, int level_);
    ~CompressionCodecOpenZL() override;

    uint8_t getMethodByte() const override;

    void updateHash(SipHash & hash) const override;

protected:
    UInt32 doCompressData(const char * source, UInt32 source_size, char * dest) const override;
    void doDecompressData(const char * source, UInt32 source_size, char * dest, UInt32 uncompressed_size) const override;

    UInt32 getMaxCompressedDataSize(UInt32 uncompressed_size) const override { 
        return static_cast<UInt32>(ZL_compressBound(uncompressed_size));
    }

    bool isCompression() const override { return true; }
    bool isGenericCompression() const override { return true; }

    String getDescription() const override
    {
        return "Uses OpenZL numeric compression";
    }


private:
    const String mode;
    const UInt8 numeric_bytes_size;
    const int level;
    ZL_Compressor* compressor;
    
    
};


namespace ErrorCodes
{
    extern const int CANNOT_COMPRESS;
    extern const int CANNOT_DECOMPRESS;
    extern const int ILLEGAL_SYNTAX_FOR_CODEC_TYPE;
    extern const int ILLEGAL_CODEC_PARAMETER;
    extern const int BAD_ARGUMENTS;
}

CompressionCodecOpenZL::CompressionCodecOpenZL(String mode_, UInt8 numeric_bytes_size_, int level_)
    : mode(mode_)
    , numeric_bytes_size(numeric_bytes_size_)
    , level(level_)
    // , log(getLogger("CompressionCodecOpenZL"), /* allowed_count */ 1, /* interval */ 1)
{
    setCodecDescription("OpenZL", {
        std::make_shared<ASTLiteral>(mode),
        std::make_shared<ASTLiteral>(static_cast<UInt64>(numeric_bytes_size)),
        std::make_shared<ASTLiteral>(static_cast<UInt64>(level)),
    });
    
    compressor = ZL_Compressor_create();
    parameterizeCompressor(compressor, level);
    ZL_GraphID graph = buildCompressor(compressor, mode, numeric_bytes_size);
    ZL_Compressor_selectStartingGraphID(compressor, graph);
}

CompressionCodecOpenZL::~CompressionCodecOpenZL()
{
    ZL_Compressor_free(compressor);

}

uint8_t CompressionCodecOpenZL::getMethodByte() const
{
    return static_cast<uint8_t>(CompressionMethodByte::OpenZL);
}

void CompressionCodecOpenZL::updateHash(SipHash & hash) const
{
    getCodecDesc()->updateTreeHash(hash, /*ignore_aliases=*/ true);
}

static constexpr int kExampleFormatVersion    = 16;
static constexpr int kExampleCompressionLevel = 3;


// --8<-- [start:setup-compressor]
void parameterizeCompressor(ZL_Compressor* compressor, int level)
{
    if (level == -1) {
        level = kExampleCompressionLevel;
    }
    ZL_Compressor_setParameter(
            compressor,
            ZL_CParam_formatVersion,
            kExampleFormatVersion);
    ZL_Compressor_setParameter(
            compressor,
            ZL_CParam_compressionLevel,
            level);
}

ZL_NodeID converterNode(UInt8 width)
{
    ZL_NodeID convert;
        switch (width) {
            case 1:
                convert = ZL_NODE_INTERPRET_AS_LE8;
                break;
            case 2:
                convert = ZL_NODE_INTERPRET_AS_LE16;
                break;
            case 4:
                convert = ZL_NODE_INTERPRET_AS_LE32;
                break;
            case 8:
                convert = ZL_NODE_INTERPRET_AS_LE64;
                break;
        }

        return convert;

}

ZL_GraphID buildFloatCompressor(ZL_Compressor* compressor, UInt8 width)
{
    std::array<ZL_GraphID, 2> successors = {
        ZL_GRAPH_FIELD_LZ, // sign+fraction
        ZL_GRAPH_FSE,   // exponent
    };
    // Separate the exponent from the sign+fraction bits.
    // Pass the exponent to FSE, and bitpack the sign+fraction bits as-is.
    return ZL_Compressor_registerStaticGraph_fromNode1o(
                compressor, converterNode(width),
    
                ZL_Compressor_registerStaticGraph_fromNode(
                    compressor,
                    ZL_NODE_FLOAT32_DECONSTRUCT,
                    successors.data(),
                    successors.size())
                );

}


ZL_GraphID buildIntCompressor(ZL_Compressor* compressor,  UInt8 width)
{
        return ZL_Compressor_registerStaticGraph_fromNode1o(
                compressor, converterNode(width), ZL_GRAPH_FIELD_LZ);
}

ZL_GraphID buildIntSortedCompressor(ZL_Compressor* compressor,  UInt8 width)
{
        return ZL_Compressor_registerStaticGraph_fromNode1o(
                compressor, converterNode(width), ZL_Compressor_registerStaticGraph_fromNode1o(
            compressor,  ZL_NODE_DELTA_INT, ZL_GRAPH_COMPRESS_GENERIC));
}

ZL_GraphID buildCompressor(ZL_Compressor* compressor, String mode, UInt8 width)
{

    if (mode == "Int") {
        return buildIntCompressor(compressor, width);
    } else if (mode == "IntSorted") {
        return buildIntSortedCompressor(compressor, width);
        // return ZL_Compressor_registerStaticGraph_fromNode1o(
        //     compressor,  ZL_NODE_DELTA_INT, ZL_GRAPH_COMPRESS_GENERIC);
    } else if (mode == "Float") {
        return buildFloatCompressor(compressor, width);
    } else {

        // todo fix
        return buildIntCompressor(compressor, width);
        // throw Exception(ErrorCodes::BAD_ARGUMENTS, "Unknown OpenZL mode {}", mode);
    }

}



UInt32 CompressionCodecOpenZL::doCompressData(const char * source, UInt32 source_size, char * dest) const
{
    // dest[0] = numeric_bytes_size;
    // TODO: error mgmt
    // Set up the compressor

    ZL_CCtx* cctx = ZL_CCtx_create();
    ZL_CCtx_refCompressor(cctx, compressor);

    // Wrap the input data as an array of native-endian numeric data
    if (source_size % numeric_bytes_size != 0) {
        int alignment = static_cast<int>(numeric_bytes_size);
        // LOG_DEBUG(log, "sizes don't match {}/{}, data: {}",source_size, numeric_bytes_size, *source);
        throw Exception(ErrorCodes::CANNOT_COMPRESS, "Cannot compress with OpenZL numeric codec, data size {} is not aligned with {}.", source_size, alignment);
    }
    // ZL_TypedRef* input =
    //         ZL_TypedRef_createNumeric(source, numeric_bytes_size, source_size / numeric_bytes_size);

    // Compress
    // const ZL_Report r = ZL_CCtx_compressTypedRef(
            // cctx, dest, ZL_compressBound(source_size), input);
    const ZL_Report r = ZL_CCtx_compress(cctx, dest, ZL_compressBound(source_size), source, source_size);
    if (ZL_isError(r)) {
        const char* msg = ZL_CCtx_getErrorContextString(cctx, r);
        throw Exception(ErrorCodes::CANNOT_COMPRESS, "Error: {}", msg);
    }

    // Cleanup
    // ZL_TypedRef_free(input);
    ZL_CCtx_free(cctx);

    return static_cast<UInt32>(ZL_validResult(r));
}

void CompressionCodecOpenZL::doDecompressData(const char * source, UInt32 source_size, char * dest, UInt32 uncompressed_size) const
{
    // ZL_TypedBuffer* output = ZL_TypedBuffer_createWrapNumeric(
    //         dest, width, uncompressed_size);

    ZL_decompress(dest, uncompressed_size, source, source_size);
    

    // Decompress
    // ZL_DCtx* dctx = ZL_DCtx_create();
    // ZL_DCtx_decompressTBuffer(dctx, output, source, source_size);
    // ZL_DCtx_decompress(dctx, dest, uncompressed_size, source, source_size);

    // Cleanup
    // ZL_TypedBuffer_free(output);
    // ZL_DCtx_free(dctx);
}

namespace
{

UInt8 getNumericBytesSize(const IDataType * column_type)
{
    if (!column_type->isValueUnambiguouslyRepresentedInFixedSizeContiguousMemoryRegion())
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Codec OpenZL is not applicable for {} because the data type is not of fixed size",
            column_type->getName());

    size_t max_size = column_type->getSizeOfValueInMemory();
    if (max_size == 1 || max_size == 2 || max_size == 4 || max_size == 8)
        return static_cast<UInt8>(max_size);
    throw Exception(
        ErrorCodes::BAD_ARGUMENTS,
        "Codec OpenZL is only applicable for data types of size 1, 2, 4, 8 bytes. Given type {}",
        column_type->getName());
}

String getDefaultMode(const IDataType * column_type)
{
    if (column_type->isValueRepresentedByInteger()) {
        return "Int";
    }

    String name = column_type->getName();
    if (name.contains("Float")) {
        return "Float";
    }

    throw Exception(ErrorCodes::ILLEGAL_CODEC_PARAMETER, "cannot infer mode from column type");
}

}

void registerCodecOpenZL(CompressionCodecFactory & factory)
{
    UInt8 method_code = static_cast<UInt8>(CompressionMethodByte::OpenZL);
    auto codec_builder = [&](const ASTPtr & arguments, const IDataType * column_type) -> CompressionCodecPtr
    {
        UInt8 numeric_bytes_size = 0;
        int level = kExampleCompressionLevel;
        String mode = "";

        if (!arguments && !column_type) 
        {
            return std::make_shared<CompressionCodecOpenZL>(mode, numeric_bytes_size, level);
        }

        if (column_type)
        {
            mode = getDefaultMode(column_type);
            numeric_bytes_size = getNumericBytesSize(column_type);
        }

        if (arguments && arguments->children.size() > 0) 
        {
            const auto children = arguments->children;
            if (children.size() != 3)
            {
                throw Exception(ErrorCodes::ILLEGAL_SYNTAX_FOR_CODEC_TYPE, "OpenZL codec must have 0 or 3 parameters, given {}", arguments->children.size());
            }

            auto * literal = children[0]->as<ASTLiteral>();
            if (!literal || literal->value.getType() != Field::Types::Which::String)
                throw Exception(ErrorCodes::ILLEGAL_CODEC_PARAMETER, "OpenZL mode argument must be string");
            
            mode = literal->value.safeGet<String>();

            literal = children[1]->as<ASTLiteral>();
            if (!literal || literal->value.getType() != Field::Types::Which::UInt64)
                throw Exception(ErrorCodes::ILLEGAL_CODEC_PARAMETER, "OpenZL size argument must be unsigned integer");

            size_t user_bytes_size = literal->value.safeGet<UInt64>();
            if (user_bytes_size != 1 && user_bytes_size != 2 && user_bytes_size != 4 && user_bytes_size != 8)
                throw Exception(ErrorCodes::ILLEGAL_CODEC_PARAMETER, "OpenZL numeric width codec can be 1, 2, 4 or 8, given {}", user_bytes_size);
            numeric_bytes_size = static_cast<UInt8>(user_bytes_size);

            literal = children[2]->as<ASTLiteral>();
            if (!literal || literal->value.getType() != Field::Types::Which::UInt64)
                throw Exception(ErrorCodes::ILLEGAL_CODEC_PARAMETER, "OpenZL level argument must be unsigned integer");

            level = literal->value.safeGet<int>();

        }

        return std::make_shared<CompressionCodecOpenZL>(mode, numeric_bytes_size, level);
    };
    factory.registerCompressionCodecWithType("OpenZL", method_code, codec_builder);
}

CompressionCodecPtr getCompressionCodecOpenZL(String mode, UInt8 numeric_bytes_size, int level)
{
    return std::make_shared<CompressionCodecOpenZL>(mode, numeric_bytes_size, level);
}

}
