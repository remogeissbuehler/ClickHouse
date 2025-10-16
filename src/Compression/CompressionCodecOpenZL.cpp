#include <Compression/ICompressionCodec.h>
#include <Compression/CompressionInfo.h>
#include <Compression/CompressionFactory.h>
#include <base/unaligned.h>
#include <Parsers/IAST.h>
#include <Parsers/ASTLiteral.h>
#include <Parsers/ASTFunction.h>

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

class CompressionCodecOpenZL : public ICompressionCodec
{
public:
    explicit CompressionCodecOpenZL(UInt8 numeric_bytes_size_, bool sorted_);

    uint8_t getMethodByte() const override;

    void updateHash(SipHash & hash) const override;

protected:
    UInt32 doCompressData(const char * source, UInt32 source_size, char * dest) const override;
    void doDecompressData(const char * source, UInt32 source_size, char * dest, UInt32 uncompressed_size) const override;

    UInt32 getMaxCompressedDataSize(UInt32 uncompressed_size) const override { 
        return static_cast<UInt32>(ZL_compressBound(uncompressed_size)) + 1;
    }

    bool isCompression() const override { return true; }
    bool isGenericCompression() const override { return true; }

    String getDescription() const override
    {
        return "Uses OpenZL numeric compression";
    }


private:
    const UInt8 numeric_bytes_size;
    const bool sorted;
};


namespace ErrorCodes
{
    extern const int CANNOT_COMPRESS;
    extern const int CANNOT_DECOMPRESS;
    extern const int ILLEGAL_SYNTAX_FOR_CODEC_TYPE;
    extern const int ILLEGAL_CODEC_PARAMETER;
    extern const int BAD_ARGUMENTS;
}

CompressionCodecOpenZL::CompressionCodecOpenZL(UInt8 numeric_bytes_size_, bool sorted_)
    : numeric_bytes_size(numeric_bytes_size_)
    , sorted(sorted_)
{
    setCodecDescription("OpenZL", {
        std::make_shared<ASTLiteral>(static_cast<UInt64>(numeric_bytes_size)),
        std::make_shared<ASTLiteral>(static_cast<UInt64>(sorted)),
    });
}

uint8_t CompressionCodecOpenZL::getMethodByte() const
{
    return static_cast<uint8_t>(CompressionMethodByte::OpenZL);
}

void CompressionCodecOpenZL::updateHash(SipHash & hash) const
{
    getCodecDesc()->updateTreeHash(hash, /*ignore_aliases=*/ true);
}

namespace {
static constexpr int kExampleFormatVersion    = 16;
static constexpr int kExampleCompressionLevel = 6;


// --8<-- [start:setup-compressor]
void parameterizeCompressor(ZL_Compressor* compressor)
{
    ZL_Compressor_setParameter(
            compressor,
            ZL_CParam_formatVersion,
            kExampleFormatVersion);
    ZL_Compressor_setParameter(
            compressor,
            ZL_CParam_compressionLevel,
            kExampleCompressionLevel);
}

// --8<-- [start:customizing-compressor1]
ZL_GraphID buildCompressorSorted(ZL_Compressor* compressor)
{
    parameterizeCompressor(compressor);
    return ZL_Compressor_registerStaticGraph_fromNode1o(
            compressor, ZL_NODE_DELTA_INT, ZL_GRAPH_COMPRESS_GENERIC);
}
// --8<-- [end:customizing-compressor1]

// --8<-- [start:customizing-compressor2]
ZL_GraphID buildCompressorInt(ZL_Compressor* compressor)
{
    parameterizeCompressor(compressor);
    return ZL_GRAPH_FIELD_LZ;
}

}

UInt32 CompressionCodecOpenZL::doCompressData(const char * source, UInt32 source_size, char * dest) const
{
    dest[0] = numeric_bytes_size;
    // TODO: error mgmt
    // Set up the compressor
    ZL_Compressor* compressor = ZL_Compressor_create();
    ZL_GraphID graph;
    if (sorted)
        graph = buildCompressorSorted(compressor);
    else
        graph = buildCompressorInt(compressor);
    ZL_Compressor_selectStartingGraphID(compressor, graph);

    ZL_CCtx* cctx = ZL_CCtx_create();
    ZL_CCtx_refCompressor(cctx, compressor);

    // Wrap the input data as an array of native-endian numeric data
    if (source_size % numeric_bytes_size != 0) {

        int alignment = static_cast<int>(numeric_bytes_size);
        throw Exception(ErrorCodes::CANNOT_COMPRESS, "Cannot compress with OpenZL numeric codec, data size {} is not aligned with {}.", source_size, alignment);
    }
    ZL_TypedRef* input =
            ZL_TypedRef_createNumeric(source, numeric_bytes_size, source_size / numeric_bytes_size);

    // Compress
    const ZL_Report r = ZL_CCtx_compressTypedRef(
            cctx, &dest[1], ZL_compressBound(source_size), input);

    // Cleanup
    ZL_TypedRef_free(input);
    ZL_CCtx_free(cctx);
    ZL_Compressor_free(compressor);

    return static_cast<UInt32>(ZL_validResult(r)) + 1;
}

void CompressionCodecOpenZL::doDecompressData(const char * source, UInt32 source_size, char * dest, UInt32 uncompressed_size) const
{
    UInt8 width = source[0];
    uncompressed_size--;
    ZL_TypedBuffer* output = ZL_TypedBuffer_createWrapNumeric(
            dest, width, uncompressed_size);

    // Decompress
    ZL_DCtx* dctx = ZL_DCtx_create();
    ZL_DCtx_decompressTBuffer(dctx, output, &source[1], source_size);

    // Cleanup
    ZL_TypedBuffer_free(output);
    ZL_DCtx_free(dctx);
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

}

void registerCodecOpenZL(CompressionCodecFactory & factory)
{
    UInt8 method_code = static_cast<UInt8>(CompressionMethodByte::OpenZL);
    auto codec_builder = [&](const ASTPtr & arguments, const IDataType * column_type) -> CompressionCodecPtr
    {
        bool sorted = false;
        UInt8 numeric_bytes_size;

        if (arguments && !arguments->children.empty())
        {
            if (arguments->children.size() > 2)
                throw Exception(ErrorCodes::ILLEGAL_SYNTAX_FOR_CODEC_TYPE, "OpenZL codec must have 2 parameter, given {}", arguments->children.size());

            const auto children = arguments->children;
            auto * literal = children[0]->as<ASTLiteral>();
            if (!literal || literal->value.getType() != Field::Types::Which::UInt64)
                throw Exception(ErrorCodes::ILLEGAL_CODEC_PARAMETER, "OpenZL size argument must be unsigned integer");

            size_t user_bytes_size = literal->value.safeGet<UInt64>();
            if (user_bytes_size != 1 && user_bytes_size != 2 && user_bytes_size != 4 && user_bytes_size != 8)
                throw Exception(ErrorCodes::ILLEGAL_CODEC_PARAMETER, "OpenZL value for delta codec can be 1, 2, 4 or 8, given {}", user_bytes_size);
            numeric_bytes_size = static_cast<UInt8>(user_bytes_size);
            if (children.size() > 1) {
                literal = children[1]->as<ASTLiteral>();
                if (!literal)
                    throw Exception(ErrorCodes::ILLEGAL_CODEC_PARAMETER, "Second OpenZL arg must be bool");

                sorted = static_cast<bool>(literal->value.safeGet<UInt64>());

            }

        }
        else if (column_type)
        {
            numeric_bytes_size = getNumericBytesSize(column_type);
        }

        return std::make_shared<CompressionCodecOpenZL>(numeric_bytes_size, sorted);
    };
    factory.registerCompressionCodecWithType("OpenZL", method_code, codec_builder);
}

CompressionCodecPtr getCompressionCodecOpenZL(UInt8 numeric_bytes_size, bool sorted)
{
    return std::make_shared<CompressionCodecOpenZL>(numeric_bytes_size, sorted);
}

}
