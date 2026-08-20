// DemucsMetal.mm — the htdemucs v4 forward pass as one compiled MPSGraph
// (issue #24's "bigger bet").
//
// Split of labour: everything demucs.cpp does OUTSIDE model_inference stays on
// the CPU and is either called directly (stft/istft, model loading) or ported
// line-for-line from model_apply.cpp (normalisation, the one random shift,
// 7.8 s segmentation with transition-weighted overlap-add) so a GPU run is
// numerically comparable to a CPU run of the same process. What runs on the
// GPU is exactly model_inference's tensor pipeline between "normalised CaC
// spectrogram + normalised waveform in" and "per-source spectrogram +
// waveform out": both encoder stacks, the frequency embedding, the channel
// up/downsamplers, the 5-pair crosstransformer, and both decoder stacks.
//
// LAYOUT CONVENTION (the #1 bug source, so it is stated once and obeyed
// everywhere): Eigen tensors are column-major (first index fastest); MPSGraph
// tensor data is row-major (last index fastest). Every buffer that crosses
// the boundary is explicitly repacked so that an Eigen (a, b, c) tensor
// becomes an MPSGraph [a, b, c] tensor — same logical indices, rewritten
// memory order — via the pack* helpers below. Graph shapes therefore read
// identically to the Eigen dims in demucs.cpp, which is what makes the
// per-depth parity checks against the CPU reference meaningful.
//
// All shapes are fixed (every segment is padded to 7.8 s before inference),
// so the graph is built and compiled once per model and then run per segment.

#if defined(__APPLE__)

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalPerformanceShadersGraph/MetalPerformanceShadersGraph.h>

#include "DemucsMetal.h"

#include <model.hpp>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

// The public entry points refuse to run below macOS 14 (gpuAvailable), so the
// MPSGraph calls beyond that floor are runtime-safe; silencing the per-call
// availability warnings here beats annotating every helper in the file.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunguarded-availability-new"

namespace
{

using demucscpp::demucs_model;

// ---- fixed geometry of a 7.8 s htdemucs segment (see model.hpp) ------------
constexpr int kSeg = 343980;         // 7.8 s * 44100
constexpr int kPad = 1536;           // floor(hop/2) * 3
constexpr int kPadEnd = 1620;        // pad + ceil(seg/hop)*hop - seg
constexpr int kPadded = kSeg + kPad + kPadEnd; // 347136
constexpr int kFrames = 336;         // FREQ_BRANCH_LEN
constexpr int kBins = 2048;          // 2049 stft bins minus the dropped top one
constexpr int kTimeLen[5] = { kSeg, 85995, 21499, 5375, 1344 };
constexpr int kFreqH[5] = { 2048, 512, 128, 32, 8 };
constexpr int kChan[5] = { 4, 48, 96, 192, 384 };  // [0] is CaC input depth
constexpr int kTxDim = 512;          // transformer model dim (4-source)
constexpr int kTxFF = 2048;          // feedforward hidden dim
constexpr int kTxHeads = 8;
constexpr int kTokF = 8 * kFrames;   // 2688 freq tokens
constexpr int kTokT = kTimeLen[4];   // 1344 time tokens
constexpr float kEps = 1e-5f;

// ---- Eigen (col-major) -> row-major repacks for graph constants ------------

std::vector<float> packT1 (const Eigen::Tensor1dXf& t)
{
    return std::vector<float> (t.data(), t.data() + t.size());
}

std::vector<float> packVec (const Eigen::VectorXf& v)
{
    return std::vector<float> (v.data(), v.data() + v.size());
}

std::vector<float> packT3 (const Eigen::Tensor3dXf& t)
{
    const auto d0 = (int) t.dimension (0), d1 = (int) t.dimension (1), d2 = (int) t.dimension (2);
    std::vector<float> out ((size_t) d0 * (size_t) d1 * (size_t) d2);
    size_t i = 0;
    for (int a = 0; a < d0; ++a)
        for (int b = 0; b < d1; ++b)
            for (int c = 0; c < d2; ++c)
                out[i++] = t (a, b, c);
    return out;
}

std::vector<float> packT4 (const Eigen::Tensor4dXf& t)
{
    const auto d0 = (int) t.dimension (0), d1 = (int) t.dimension (1),
               d2 = (int) t.dimension (2), d3 = (int) t.dimension (3);
    std::vector<float> out ((size_t) d0 * (size_t) d1 * (size_t) d2 * (size_t) d3);
    size_t i = 0;
    for (int a = 0; a < d0; ++a)
        for (int b = 0; b < d1; ++b)
            for (int c = 0; c < d2; ++c)
                for (int d = 0; d < d3; ++d)
                    out[i++] = t (a, b, c, d);
    return out;
}

// A col-major (R, C) matrix read as-is IS the row-major [C, R] transpose, and
// every transformer matmul here wants x * W^T — so the raw buffer uploads
// unchanged with shape [C, R]. No repack, by construction rather than luck.
std::vector<float> packMatT (const Eigen::MatrixXf& m)
{
    return std::vector<float> (m.data(), m.data() + m.size());
}

// ---- positional embeddings, ported verbatim from crosstransformer.cpp -----
// (they are file-static upstream, so the maths is duplicated here; shapes are
// fixed, so both are baked into the graph as constants.)

Eigen::Tensor3dXf create2dSinEmbedding (int dModel, int height, int width, float maxPeriod = 10000.0f)
{
    Eigen::Tensor3dXf pe (dModel, height, width);
    dModel /= 2;
    Eigen::ArrayXf divTerm =
        Eigen::exp (Eigen::ArrayXf::LinSpaced (dModel / 2, 0.0f, (float) (dModel - 2))
                    * (-std::log (maxPeriod) / (float) dModel));

    for (int i = 0; i < width; ++i)
        for (int j = 0; j < dModel / 2; ++j)
        {
            const float valW = (float) i * divTerm (j);
            pe.slice (Eigen::array<int, 3> ({ j * 2, 0, i }), Eigen::array<int, 3> ({ 1, height, 1 }))
                .setConstant (std::sin (valW));
            pe.slice (Eigen::array<int, 3> ({ j * 2 + 1, 0, i }), Eigen::array<int, 3> ({ 1, height, 1 }))
                .setConstant (std::cos (valW));
        }

    for (int i = 0; i < height; ++i)
        for (int j = 0; j < dModel / 2; ++j)
        {
            const float valH = (float) i * divTerm (j);
            pe.slice (Eigen::array<int, 3> ({ dModel + j * 2, i, 0 }), Eigen::array<int, 3> ({ 1, 1, width }))
                .setConstant (std::sin (valH));
            pe.slice (Eigen::array<int, 3> ({ dModel + j * 2 + 1, i, 0 }), Eigen::array<int, 3> ({ 1, 1, width }))
                .setConstant (std::cos (valH));
        }

    return pe;
}

std::vector<float> makePosEmbed2d() // row-major [kTokF][kTxDim], token = t*8 + f
{
    const auto pe = create2dSinEmbedding (kTxDim, 8, kFrames);
    std::vector<float> out ((size_t) kTokF * kTxDim);
    for (int f = 0; f < 8; ++f)
        for (int t = 0; t < kFrames; ++t)
            for (int c = 0; c < kTxDim; ++c)
                out[(size_t) ((t * 8 + f) * kTxDim + c)] = pe (c, f, t);
    return out;
}

std::vector<float> makePosEmbed1d() // row-major [kTokT][kTxDim]
{
    std::vector<float> out ((size_t) kTokT * kTxDim);
    const int half = kTxDim / 2;
    Eigen::ArrayXf divTerm = Eigen::ArrayXf::LinSpaced (half, 0.0f, (float) (half - 1)) / (float) (half - 1);

    for (int t = 0; t < kTokT; ++t)
        for (int i = 0; i < half; ++i)
        {
            const float phase = (float) t / std::pow (10000.0f, divTerm (i));
            out[(size_t) (t * kTxDim + i)] = std::cos (phase);          // first half: cos
            out[(size_t) (t * kTxDim + i + half)] = std::sin (phase);   // second half: sin
        }
    return out;
}

// ---- the engine ------------------------------------------------------------

class MetalHtDemucs
{
public:
    explicit MetalHtDemucs (const demucs_model& model) { build (model); }

    bool valid() const { return exe != nil; }
    NSString* buildError() const { return buildErr; }

    // Feeds one normalised segment (row-major, shapes in the header) and reads
    // back the two outputs. Returns false on any Metal failure.
    bool run (const float* spec, const float* wave, float* specOutHost, float* waveOutHost, std::string& err)
    {
        @autoreleasepool
        {
            @try
            {
                std::memcpy (specBuf.contents, spec, specBytes);
                std::memcpy (waveBuf.contents, wave, waveBytes);

                NSMutableArray<MPSGraphTensorData*>* inputs = [NSMutableArray array];
                for (MPSGraphTensor* t in exe.feedTensors)
                    [inputs addObject: (t == specIn ? specTD : waveTD)];

                NSArray<MPSGraphTensorData*>* results =
                    [exe runWithMTLCommandQueue: queue inputsArray: inputs resultsArray: nil executionDescriptor: nil];

                if (results.count != 2)
                {
                    err = "MPSGraph returned an unexpected result count";
                    return false;
                }

                for (NSUInteger i = 0; i < results.count; ++i)
                {
                    MPSGraphTensor* t = exe.targetTensors[i];
                    [[results[i] mpsndarray] readBytes: (t == specOut ? specOutHost : waveOutHost) strideBytes: nil];
                }
                return true;
            }
            @catch (NSException* e)
            {
                err = std::string ("MPSGraph run failed: ") + e.reason.UTF8String;
                return false;
            }
        }
    }

    // Validation path: uncompiled per-call execution so any recorded tap can
    // be a target without recompiling the production executable.
    bool runTaps (const float* spec, const float* wave,
                  const std::vector<std::string>& names,
                  std::vector<std::vector<float>>& out, std::string& err)
    {
        @autoreleasepool
        {
            @try
            {
                std::memcpy (specBuf.contents, spec, specBytes);
                std::memcpy (waveBuf.contents, wave, waveBytes);

                NSMutableArray<MPSGraphTensor*>* targets = [NSMutableArray array];
                for (const auto& n : names)
                {
                    NSString* key = [NSString stringWithUTF8String: n.c_str()];
                    MPSGraphTensor* t = key != nil ? taps[key] : nil;
                    if (t == nil)
                    {
                        err = "unknown tap: " + n;
                        return false;
                    }
                    [targets addObject: t];
                }

                NSDictionary* res = [graph runWithMTLCommandQueue: queue
                                                            feeds: @{ specIn: specTD, waveIn: waveTD }
                                                    targetTensors: targets
                                                 targetOperations: nil];
                out.clear();
                for (MPSGraphTensor* t in targets)
                {
                    MPSGraphTensorData* td = res[t];
                    NSUInteger n = 1;
                    for (NSNumber* d in td.shape)
                        n *= d.unsignedIntegerValue;
                    std::vector<float> v (n);
                    [[td mpsndarray] readBytes: v.data() strideBytes: nil];
                    out.push_back (std::move (v));
                }
                return true;
            }
            @catch (NSException* e)
            {
                err = std::string ("MPSGraph tap run failed: ") + e.reason.UTF8String;
                return false;
            }
        }
    }

private:
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    MPSGraph* graph = nil;
    MPSGraphExecutable* exe = nil;
    MPSGraphTensor *specIn = nil, *waveIn = nil, *specOut = nil, *waveOut = nil;
    NSMutableDictionary<NSString*, MPSGraphTensor*>* taps = nil;
    id<MTLBuffer> specBuf = nil, waveBuf = nil;
    MPSGraphTensorData *specTD = nil, *waveTD = nil;
    NSString* buildErr = nil;
    size_t specBytes = 0, waveBytes = 0;

    // -- graph-building vocabulary --

    static NSArray<NSNumber*>* shape4 (int a, int b, int c, int d)
    {
        return @[@(a), @(b), @(c), @(d)];
    }

    MPSGraphTensor* constant (const std::vector<float>& data, NSArray<NSNumber*>* shp)
    {
        NSUInteger n = 1;
        for (NSNumber* d in shp)
            n *= d.unsignedIntegerValue;
        NSCAssert (n == data.size(), @"constant size mismatch");
        return [graph constantWithData: [NSData dataWithBytes: data.data() length: data.size() * sizeof (float)]
                                 shape: shp
                              dataType: MPSDataTypeFloat32];
    }

    MPSGraphTensor* scalar (float v) { return [graph constantWithScalar: v shape: @[@1] dataType: MPSDataTypeFloat32]; }

    MPSGraphTensor* add (MPSGraphTensor* a, MPSGraphTensor* b) { return [graph additionWithPrimaryTensor: a secondaryTensor: b name: nil]; }
    MPSGraphTensor* sub (MPSGraphTensor* a, MPSGraphTensor* b) { return [graph subtractionWithPrimaryTensor: a secondaryTensor: b name: nil]; }
    MPSGraphTensor* mul (MPSGraphTensor* a, MPSGraphTensor* b) { return [graph multiplicationWithPrimaryTensor: a secondaryTensor: b name: nil]; }
    MPSGraphTensor* div (MPSGraphTensor* a, MPSGraphTensor* b) { return [graph divisionWithPrimaryTensor: a secondaryTensor: b name: nil]; }
    MPSGraphTensor* mm (MPSGraphTensor* a, MPSGraphTensor* b) { return [graph matrixMultiplicationWithPrimaryTensor: a secondaryTensor: b name: nil]; }
    MPSGraphTensor* reshape (MPSGraphTensor* x, NSArray<NSNumber*>* s) { return [graph reshapeTensor: x withShape: s name: nil]; }
    MPSGraphTensor* transpose (MPSGraphTensor* x, NSUInteger a, NSUInteger b) { return [graph transposeTensor: x dimension: a withDimension: b name: nil]; }
    MPSGraphTensor* slice (MPSGraphTensor* x, NSUInteger dim, NSInteger start, NSInteger len)
    {
        return [graph sliceTensor: x dimension: dim start: start length: len name: nil];
    }

    void tap (const char* name, MPSGraphTensor* t)
    {
        NSString* key = [NSString stringWithUTF8String: name];
        if (key != nil)
            taps[key] = t;
    }

    // Exact (erf-based) GELU, matching demucscpp::gelu.
    MPSGraphTensor* gelu (MPSGraphTensor* x)
    {
        MPSGraphTensor* e = [graph erfWithTensor: mul (x, scalar (0.70710678118654752f)) name: nil];
        return mul (mul (x, add (e, scalar (1.0f))), scalar (0.5f));
    }

    MPSGraphTensor* conv (MPSGraphTensor* x, MPSGraphTensor* w, MPSGraphTensor* b,
                          int sy, int sx, int pt, int pb, int pl, int pr, int dy = 1, int dx = 1)
    {
        MPSGraphConvolution2DOpDescriptor* d = [MPSGraphConvolution2DOpDescriptor
            descriptorWithStrideInX: (NSUInteger) sx strideInY: (NSUInteger) sy
                    dilationRateInX: (NSUInteger) dx dilationRateInY: (NSUInteger) dy
                             groups: 1
                        paddingLeft: (NSUInteger) pl paddingRight: (NSUInteger) pr
                         paddingTop: (NSUInteger) pt paddingBottom: (NSUInteger) pb
                       paddingStyle: MPSGraphPaddingStyleExplicit
                         dataLayout: MPSGraphTensorNamedDataLayoutNCHW
                      weightsLayout: MPSGraphTensorNamedDataLayoutOIHW];
        MPSGraphTensor* y = [graph convolution2DWithSourceTensor: x weightsTensor: w descriptor: d name: nil];
        return b != nil ? add (y, b) : y;
    }

    // Transposed conv. MPSGraph's OIHW here is the *forward* conv's layout,
    // which is exactly PyTorch's ConvTranspose (in, out, kh, kw) — so demucs
    // conv_tr weights upload with no axis swap (verified numerically against
    // a naive loop before this file was written).
    MPSGraphTensor* convT (MPSGraphTensor* x, MPSGraphTensor* w, MPSGraphTensor* b,
                           int sy, int sx, NSArray<NSNumber*>* outShape)
    {
        MPSGraphConvolution2DOpDescriptor* d = [MPSGraphConvolution2DOpDescriptor
            descriptorWithStrideInX: (NSUInteger) sx strideInY: (NSUInteger) sy
                    dilationRateInX: 1 dilationRateInY: 1
                             groups: 1
                        paddingLeft: 0 paddingRight: 0 paddingTop: 0 paddingBottom: 0
                       paddingStyle: MPSGraphPaddingStyleExplicit
                         dataLayout: MPSGraphTensorNamedDataLayoutNCHW
                      weightsLayout: MPSGraphTensorNamedDataLayoutOIHW];
        MPSGraphTensor* y = [graph convolutionTranspose2DWithSourceTensor: x weightsTensor: w outputShape: outShape descriptor: d name: nil];
        return b != nil ? add (y, b) : y;
    }

    // GLU over the channel axis of [N, 2C, H, W].
    MPSGraphTensor* gluChannels (MPSGraphTensor* x, int c)
    {
        MPSGraphTensor* a = slice (x, 1, 0, c);
        MPSGraphTensor* g = slice (x, 1, c, c);
        return mul (a, [graph sigmoidWithTensor: g name: nil]);
    }

    // demucs.cpp's group_norm with num_groups == 1: per batch item, normalise
    // over everything else. Mirrors its unbiased variance (N-1), which is NOT
    // what PyTorch does but IS what the CPU reference computes.
    MPSGraphTensor* groupNormAll (MPSGraphTensor* x, MPSGraphTensor* w, MPSGraphTensor* b, double count, bool fuseGelu)
    {
        NSArray<NSNumber*>* axes = @[@1, @2, @3];
        MPSGraphTensor* m = [graph meanOfTensor: x axes: axes name: nil];
        MPSGraphTensor* d = sub (x, m);
        MPSGraphTensor* var = div ([graph reductionSumWithTensor: mul (d, d) axes: axes name: nil],
                                   scalar ((float) (count - 1.0)));
        MPSGraphTensor* y = div (d, [graph squareRootWithTensor: add (var, scalar (kEps)) name: nil]);
        y = add (mul (y, w), b);
        return fuseGelu ? gelu (y) : y;
    }

    // demucs.cpp's layer_norm: per row of [T, C], normalise over C.
    MPSGraphTensor* layerNorm (MPSGraphTensor* x, MPSGraphTensor* w, MPSGraphTensor* b, int c)
    {
        NSArray<NSNumber*>* axes = @[@1];
        MPSGraphTensor* m = [graph meanOfTensor: x axes: axes name: nil];
        MPSGraphTensor* d = sub (x, m);
        MPSGraphTensor* var = div ([graph reductionSumWithTensor: mul (d, d) axes: axes name: nil],
                                   scalar ((float) (c - 1)));
        MPSGraphTensor* y = div (d, [graph squareRootWithTensor: add (var, scalar (kEps)) name: nil]);
        return add (mul (y, w), b);
    }

    // -- DConv residual branch (layers.cpp apply_dconv), on [N, C, 1, T] --

    struct DconvConsts
    {
        MPSGraphTensor *c0w[2], *c0b[2], *gn1w[2], *gn1b[2];
        MPSGraphTensor *c3w[2], *c3b[2], *gn4w[2], *gn4b[2], *scale[2];
    };

    DconvConsts dconvConsts (const demucs_model& m, int freqIdx, int encdecIdx, int layerIdx)
    {
        DconvConsts c;
        const int ch = kChan[layerIdx + 1];
        const int hid = ch / 8;

        for (int blk = 0; blk < 2; ++blk)
        {
            c.c0w[blk] = constant (packT3 (m.dconv_layers_0_conv1d_weight[freqIdx][encdecIdx][layerIdx][blk]),
                                   shape4 (hid, ch, 1, 3));
            c.c0b[blk] = constant (packT1 (m.dconv_layers_0_conv1d_bias[freqIdx][encdecIdx][layerIdx][blk]),
                                   shape4 (1, hid, 1, 1));
            c.gn1w[blk] = constant (packT1 (m.dconv_layers_1_groupnorm_weight[freqIdx][encdecIdx][layerIdx][blk]),
                                    shape4 (1, hid, 1, 1));
            c.gn1b[blk] = constant (packT1 (m.dconv_layers_1_groupnorm_bias[freqIdx][encdecIdx][layerIdx][blk]),
                                    shape4 (1, hid, 1, 1));
            c.c3w[blk] = constant (packT3 (m.dconv_layers_3_conv1d_weight[freqIdx][encdecIdx][layerIdx][blk]),
                                   shape4 (2 * ch, hid, 1, 1));
            c.c3b[blk] = constant (packT1 (m.dconv_layers_3_conv1d_bias[freqIdx][encdecIdx][layerIdx][blk]),
                                   shape4 (1, 2 * ch, 1, 1));
            c.gn4w[blk] = constant (packT1 (m.dconv_layers_4_groupnorm_weight[freqIdx][encdecIdx][layerIdx][blk]),
                                    shape4 (1, 2 * ch, 1, 1));
            c.gn4b[blk] = constant (packT1 (m.dconv_layers_4_groupnorm_bias[freqIdx][encdecIdx][layerIdx][blk]),
                                    shape4 (1, 2 * ch, 1, 1));
            c.scale[blk] = constant (packT1 (m.dconv_layers_6_scale[freqIdx][encdecIdx][layerIdx][blk]),
                                     shape4 (1, ch, 1, 1));
        }
        return c;
    }

    MPSGraphTensor* dconv (MPSGraphTensor* x, const DconvConsts& c, int ch, int t)
    {
        const int hid = ch / 8;
        for (int blk = 0; blk < 2; ++blk)
        {
            // block 0: k3 s1 p1 d1; block 1: k3 s1 p2 d2 — both length-preserving,
            // so the reference's mid_crop is a no-op at these fixed shapes.
            const int p = blk == 0 ? 1 : 2, dil = blk == 0 ? 1 : 2;
            MPSGraphTensor* y = conv (x, c.c0w[blk], c.c0b[blk], 1, 1, 0, 0, p, p, 1, dil);
            y = groupNormAll (y, c.gn1w[blk], c.gn1b[blk], (double) hid * t, true);
            y = conv (y, c.c3w[blk], c.c3b[blk], 1, 1, 0, 0, 0, 0);
            y = groupNormAll (y, c.gn4w[blk], c.gn4b[blk], (double) 2 * ch * t, false);
            y = gluChannels (y, ch);
            y = mul (y, c.scale[blk]);
            x = add (x, y);
        }
        return x;
    }

    // Freq-branch dconv folds frequency into the batch, exactly like the
    // reference's (freq, chan, time) shuffle: [1,C,F,T] -> [F,C,1,T] -> back.
    MPSGraphTensor* dconvFreqBatched (MPSGraphTensor* x, const DconvConsts& c, int ch, int f, int t)
    {
        MPSGraphTensor* y = transpose (x, 1, 2);           // [1, F, C, T]
        y = reshape (y, shape4 (f, ch, 1, t));
        y = dconv (y, c, ch, t);
        y = reshape (y, shape4 (1, f, ch, t));
        return transpose (y, 1, 2);                        // [1, C, F, T]
    }

    // -- transformer layer (layers.cpp common_encoder_layer) --

    struct TxConsts
    {
        MPSGraphTensor *n1w, *n1b, *n2w, *n2b;   // pre-attn norms (n2: cross only)
        MPSGraphTensor *inWT, *inB;              // [C, 3C], [3C]
        MPSGraphTensor *outWT, *outB, *g1;
        MPSGraphTensor *n3w, *n3b;               // pre-FF norm
        MPSGraphTensor *l1T, *l1b, *l2T, *l2b, *g2;
        MPSGraphTensor *noW, *noB;               // whole-tensor norm_out
    };

    MPSGraphTensor* txLayer (MPSGraphTensor* q, MPSGraphTensor* k, bool selfAttn,
                             const TxConsts& c, int tq, int tk)
    {
        const int C = kTxDim, hd = C / kTxHeads;

        MPSGraphTensor* qn = layerNorm (q, c.n1w, c.n1b, C);
        MPSGraphTensor* kn = selfAttn ? qn : layerNorm (k, c.n2w, c.n2b, C);

        MPSGraphTensor* Q = add (mm (qn, slice (c.inWT, 1, 0, C)), slice (c.inB, 0, 0, C));
        MPSGraphTensor* K = add (mm (kn, slice (c.inWT, 1, C, C)), slice (c.inB, 0, C, C));
        MPSGraphTensor* V = add (mm (kn, slice (c.inWT, 1, 2 * C, C)), slice (c.inB, 0, 2 * C, C));

        MPSGraphTensor* Qh = transpose (reshape (Q, @[@(tq), @(kTxHeads), @(hd)]), 0, 1); // [H, Tq, hd]
        MPSGraphTensor* Kh = transpose (reshape (K, @[@(tk), @(kTxHeads), @(hd)]), 0, 1);
        MPSGraphTensor* Vh = transpose (reshape (V, @[@(tk), @(kTxHeads), @(hd)]), 0, 1);

        MPSGraphTensor* scores = mul (mm (Qh, transpose (Kh, 1, 2)), scalar (1.0f / std::sqrt ((float) hd)));
        MPSGraphTensor* probs = [graph softMaxWithTensor: scores axis: 2 name: nil];
        MPSGraphTensor* attn = mm (probs, Vh);                                             // [H, Tq, hd]
        attn = reshape (transpose (attn, 0, 1), @[@(tq), @(C)]);

        MPSGraphTensor* o = add (mm (attn, c.outWT), c.outB);
        q = add (q, mul (o, c.g1));

        MPSGraphTensor* f = layerNorm (q, c.n3w, c.n3b, C);
        f = gelu (add (mm (f, c.l1T), c.l1b));
        f = add (mm (f, c.l2T), c.l2b);
        q = add (q, mul (f, c.g2));

        // norm_out is MyGroupNorm(1, C) on (1, C, T): one mean/var across the
        // whole (T, C) slab, then per-channel affine.
        NSArray<NSNumber*>* axes = @[@0, @1];
        MPSGraphTensor* m = [graph meanOfTensor: q axes: axes name: nil];
        MPSGraphTensor* d = sub (q, m);
        MPSGraphTensor* var = div ([graph reductionSumWithTensor: mul (d, d) axes: axes name: nil],
                                   scalar ((float) ((double) tq * C - 1.0)));
        MPSGraphTensor* y = div (d, [graph squareRootWithTensor: add (var, scalar (kEps)) name: nil]);
        return add (mul (y, c.noW), c.noB);
    }

    MPSGraphTensor* t1 (const Eigen::Tensor1dXf& t) { return constant (packT1 (t), @[@((int) t.size())]); }
    MPSGraphTensor* v1 (const Eigen::VectorXf& v) { return constant (packVec (v), @[@((int) v.size())]); }

    TxConsts txSelfConsts (const demucscpp::crosstransformer_base& t, int branch, int idx)
    {
        TxConsts c {};
        c.n1w = t1 (t.crosstransformer_my_layers_norm1_weight[branch][idx]);
        c.n1b = t1 (t.crosstransformer_my_layers_norm1_bias[branch][idx]);
        c.inWT = constant (packMatT (t.crosstransformer_my_layers_self_attn_in_proj_weight[branch][idx]),
                           @[@(kTxDim), @(3 * kTxDim)]);
        c.inB = v1 (t.crosstransformer_my_layers_self_attn_in_proj_bias[branch][idx]);
        c.outWT = constant (packMatT (t.crosstransformer_my_layers_self_attn_out_proj_weight[branch][idx]),
                            @[@(kTxDim), @(kTxDim)]);
        c.outB = v1 (t.crosstransformer_my_layers_self_attn_out_proj_bias[branch][idx]);
        c.g1 = v1 (t.crosstransformer_my_layers_gamma_1_scale[branch][idx]);
        c.n3w = t1 (t.crosstransformer_my_layers_norm2_weight[branch][idx]); // norm2 IS the pre-FF norm here
        c.n3b = t1 (t.crosstransformer_my_layers_norm2_bias[branch][idx]);
        c.l1T = constant (packMatT (t.crosstransformer_my_layers_linear1_weight[branch][idx]),
                          @[@(kTxDim), @(kTxFF)]);
        c.l1b = v1 (t.crosstransformer_my_layers_linear1_bias[branch][idx]);
        c.l2T = constant (packMatT (t.crosstransformer_my_layers_linear2_weight[branch][idx]),
                          @[@(kTxFF), @(kTxDim)]);
        c.l2b = v1 (t.crosstransformer_my_layers_linear2_bias[branch][idx]);
        c.g2 = v1 (t.crosstransformer_my_layers_gamma_2_scale[branch][idx]);
        c.noW = t1 (t.crosstransformer_my_layers_norm_out_weight[branch][idx]);
        c.noB = t1 (t.crosstransformer_my_layers_norm_out_bias[branch][idx]);
        return c;
    }

    TxConsts txCrossConsts (const demucscpp::crosstransformer_base& t, int branch, int idx)
    {
        TxConsts c {};
        c.n1w = t1 (t.crosstransformer_cross_layers_norm1_weight[branch][idx]);
        c.n1b = t1 (t.crosstransformer_cross_layers_norm1_bias[branch][idx]);
        c.n2w = t1 (t.crosstransformer_cross_layers_norm2_weight[branch][idx]);
        c.n2b = t1 (t.crosstransformer_cross_layers_norm2_bias[branch][idx]);
        c.inWT = constant (packMatT (t.crosstransformer_cross_layers_cross_attn_in_proj_weight[branch][idx]),
                           @[@(kTxDim), @(3 * kTxDim)]);
        c.inB = v1 (t.crosstransformer_cross_layers_cross_attn_in_proj_bias[branch][idx]);
        c.outWT = constant (packMatT (t.crosstransformer_cross_layers_cross_attn_out_proj_weight[branch][idx]),
                            @[@(kTxDim), @(kTxDim)]);
        c.outB = v1 (t.crosstransformer_cross_layers_cross_attn_out_proj_bias[branch][idx]);
        c.g1 = v1 (t.crosstransformer_cross_layers_gamma_1_scale[branch][idx]);
        c.n3w = t1 (t.crosstransformer_cross_layers_norm3_weight[branch][idx]);
        c.n3b = t1 (t.crosstransformer_cross_layers_norm3_bias[branch][idx]);
        c.l1T = constant (packMatT (t.crosstransformer_cross_layers_linear1_weight[branch][idx]),
                          @[@(kTxDim), @(kTxFF)]);
        c.l1b = v1 (t.crosstransformer_cross_layers_linear1_bias[branch][idx]);
        c.l2T = constant (packMatT (t.crosstransformer_cross_layers_linear2_weight[branch][idx]),
                          @[@(kTxFF), @(kTxDim)]);
        c.l2b = v1 (t.crosstransformer_cross_layers_linear2_bias[branch][idx]);
        c.g2 = v1 (t.crosstransformer_cross_layers_gamma_2_scale[branch][idx]);
        c.noW = t1 (t.crosstransformer_cross_layers_norm_out_weight[branch][idx]);
        c.noB = t1 (t.crosstransformer_cross_layers_norm_out_bias[branch][idx]);
        return c;
    }

    // -- the whole forward pass --

    void build (const demucs_model& m)
    {
        @autoreleasepool
        {
            @try
            {
                device = MTLCreateSystemDefaultDevice();
                if (device == nil)
                {
                    buildErr = @"no Metal device";
                    return;
                }
                queue = [device newCommandQueue];
                graph = [MPSGraph new];
                taps = [NSMutableDictionary dictionary];

                specIn = [graph placeholderWithShape: shape4 (1, 4, kBins, kFrames) dataType: MPSDataTypeFloat32 name: @"spec"];
                waveIn = [graph placeholderWithShape: shape4 (1, 2, 1, kSeg) dataType: MPSDataTypeFloat32 name: @"wave"];

                auto* ct4s = static_cast<demucscpp::demucs_crosstransformer_4s*> (m.crosstransformer.get());

                // ---- encoders --------------------------------------------
                MPSGraphTensor* x = specIn;
                MPSGraphTensor* xt = waveIn;
                MPSGraphTensor* saved[4];
                MPSGraphTensor* savedt[4];

                for (int i = 0; i < 4; ++i)
                {
                    const int ci = kChan[i];                   // freq in-depth: 4, 48, 96, 192
                    const int co = kChan[i + 1];
                    const int fOut = kFreqH[i + 1];

                    // freq: conv (8,1)/(4,1)/pad(2,0) + GELU, dconv over time
                    // with freq folded to batch, 1x1 rewrite, channel GLU
                    MPSGraphTensor* w = constant (packT3 (m.encoder_conv_weight[i]), shape4 (co, ci, 8, 1));
                    MPSGraphTensor* b = constant (packT1 (m.encoder_conv_bias[i]), shape4 (1, co, 1, 1));
                    MPSGraphTensor* y = gelu (conv (x, w, b, 4, 1, 2, 2, 0, 0));

                    y = dconvFreqBatched (y, dconvConsts (m, 0, 0, i), co, fOut, kFrames);

                    MPSGraphTensor* rw = constant (packT3 (m.encoder_rewrite_weight[i]), shape4 (2 * co, co, 1, 1));
                    MPSGraphTensor* rb = constant (packT1 (m.encoder_rewrite_bias[i]), shape4 (1, 2 * co, 1, 1));
                    x = gluChannels (conv (y, rw, rb, 1, 1, 0, 0, 0, 0), co);

                    if (i == 0)
                    {
                        // frequency embedding, both scale factors absorbed
                        // (10 * 0.2), added to every frame like the reference.
                        std::vector<float> emb ((size_t) 48 * 512);
                        for (int c2 = 0; c2 < 48; ++c2)
                            for (int f = 0; f < 512; ++f)
                                emb[(size_t) (c2 * 512 + f)] = m.freq_emb_embedding_weight (f, c2) * 2.0f;
                        x = add (x, constant (emb, shape4 (1, 48, 512, 1)));
                    }

                    saved[i] = x;
                    tap (("x" + std::to_string (i)).c_str(), x);

                    // time: conv (1,8)/(1,4) with the reference's ceil-mode
                    // right padding, then the same dconv/rewrite/GLU shape
                    const int ti = i == 0 ? 2 : kChan[i];
                    const int lIn = kTimeLen[i], lOut = kTimeLen[i + 1];
                    const int padRight = (lIn + 4 - 8) % 4 == 0 ? 2 : 2 + (4 - (lIn + 4 - 8) % 4);

                    MPSGraphTensor* tw = constant (packT3 (m.tencoder_conv_weight[i]), shape4 (co, ti, 1, 8));
                    MPSGraphTensor* tb = constant (packT1 (m.tencoder_conv_bias[i]), shape4 (1, co, 1, 1));
                    MPSGraphTensor* yt = gelu (conv (xt, tw, tb, 1, 4, 0, 0, 2, padRight));

                    yt = dconv (yt, dconvConsts (m, 1, 0, i), co, lOut);

                    MPSGraphTensor* trw = constant (packT3 (m.tencoder_rewrite_weight[i]), shape4 (2 * co, co, 1, 1));
                    MPSGraphTensor* trb = constant (packT1 (m.tencoder_rewrite_bias[i]), shape4 (1, 2 * co, 1, 1));
                    xt = gluChannels (conv (yt, trw, trb, 1, 1, 0, 0, 0, 0), co);

                    savedt[i] = xt;
                    tap (("xt" + std::to_string (i)).c_str(), xt);
                }

                // ---- channel upsample + crosstransformer -----------------
                MPSGraphTensor* upw = constant (packT3 (ct4s->channel_upsampler_weight), shape4 (512, 384, 1, 1));
                MPSGraphTensor* upb = constant (packT1 (ct4s->channel_upsampler_bias), shape4 (1, 512, 1, 1));
                MPSGraphTensor* xu = conv (x, upw, upb, 1, 1, 0, 0, 0, 0);      // [1,512,8,336]

                MPSGraphTensor* uptw = constant (packT3 (ct4s->channel_upsampler_t_weight), shape4 (512, 384, 1, 1));
                MPSGraphTensor* uptb = constant (packT1 (ct4s->channel_upsampler_t_bias), shape4 (1, 512, 1, 1));
                MPSGraphTensor* xtu = conv (xt, uptw, uptb, 1, 1, 0, 0, 0, 0);  // [1,512,1,1344]

                // tokens: "b c fr t -> (t fr) c", i.e. token index = t*8 + f:
                // [1,512,8,336] -> swap dims 1,3 -> [1,336,8,512] -> flatten
                MPSGraphTensor* xf = reshape (transpose (xu, 1, 3), @[@(kTokF), @(kTxDim)]);
                MPSGraphTensor* xtf = reshape (transpose (reshape (xtu, @[@1, @(kTxDim), @(kTokT)]), 1, 2),
                                               @[@(kTokT), @(kTxDim)]);

                xf = add (layerNorm (xf, t1 (ct4s->crosstransformer_norm_in_weight),
                                     t1 (ct4s->crosstransformer_norm_in_bias), kTxDim),
                          constant (makePosEmbed2d(), @[@(kTokF), @(kTxDim)]));
                xtf = add (layerNorm (xtf, t1 (ct4s->crosstransformer_norm_in_t_weight),
                                      t1 (ct4s->crosstransformer_norm_in_t_bias), kTxDim),
                           constant (makePosEmbed1d(), @[@(kTokT), @(kTxDim)]));

                // 5 layer pairs: self, cross, self, cross, self — with the
                // cross pair reading each other's PRE-cross value, exactly as
                // crosstransformer.cpp sequences it (old_x copies).
                for (int pair = 0; pair < 2; ++pair)
                {
                    xf = txLayer (xf, xf, true, txSelfConsts (*ct4s, 0, pair), kTokF, kTokF);
                    xtf = txLayer (xtf, xtf, true, txSelfConsts (*ct4s, 1, pair), kTokT, kTokT);

                    MPSGraphTensor* oldX = xf;
                    xf = txLayer (xf, xtf, false, txCrossConsts (*ct4s, 0, pair), kTokF, kTokT);
                    xtf = txLayer (xtf, oldX, false, txCrossConsts (*ct4s, 1, pair), kTokT, kTokF);
                }
                xf = txLayer (xf, xf, true, txSelfConsts (*ct4s, 0, 2), kTokF, kTokF);
                xtf = txLayer (xtf, xtf, true, txSelfConsts (*ct4s, 1, 2), kTokT, kTokT);

                // ---- channel downsample back to the branch layouts -------
                MPSGraphTensor* dnw = constant (packT3 (ct4s->channel_downsampler_weight), shape4 (384, 512, 1, 1));
                MPSGraphTensor* dnb = constant (packT1 (ct4s->channel_downsampler_bias), shape4 (1, 384, 1, 1));
                x = reshape (transpose (xf, 0, 1), shape4 (1, kTxDim, 1, kTokF));
                x = conv (x, dnw, dnb, 1, 1, 0, 0, 0, 0);
                x = transpose (reshape (x, shape4 (1, 384, kFrames, 8)), 2, 3); // undo (t fr) token order

                MPSGraphTensor* dntw = constant (packT3 (ct4s->channel_downsampler_t_weight), shape4 (384, 512, 1, 1));
                MPSGraphTensor* dntb = constant (packT1 (ct4s->channel_downsampler_t_bias), shape4 (1, 384, 1, 1));
                xt = reshape (transpose (xtf, 0, 1), shape4 (1, kTxDim, 1, kTokT));
                xt = conv (xt, dntw, dntb, 1, 1, 0, 0, 0, 0);

                tap ("x3d", x);
                tap ("xt3d", xt);

                // ---- decoders --------------------------------------------
                for (int i = 0; i < 4; ++i)
                {
                    const int ci = kChan[4 - i];               // 384, 192, 96, 48
                    const int co = i < 3 ? kChan[3 - i] : 16;  // freq out; final = 4 sources * CaC
                    const int f = kFreqH[4 - i], t = kFrames;

                    // freq: skip add, 3x3 rewrite + channel GLU, dconv,
                    // (8,1)-stride-(4,1) transposed conv, crop 2 bins each end
                    MPSGraphTensor* y = add (x, saved[3 - i]);
                    MPSGraphTensor* rw = constant (packT4 (m.decoder_rewrite_weight[i]), shape4 (2 * ci, ci, 3, 3));
                    MPSGraphTensor* rb = constant (packT1 (m.decoder_rewrite_bias[i]), shape4 (1, 2 * ci, 1, 1));
                    y = gluChannels (conv (y, rw, rb, 1, 1, 1, 1, 1, 1), ci);

                    y = dconvFreqBatched (y, dconvConsts (m, 0, 1, 3 - i), ci, f, t);

                    MPSGraphTensor* tw = constant (packT4 (m.decoder_conv_tr_weight[i]), shape4 (ci, co, 8, 1));
                    MPSGraphTensor* tb = constant (packT1 (m.decoder_conv_tr_bias[i]), shape4 (1, co, 1, 1));
                    y = convT (y, tw, tb, 4, 1, shape4 (1, co, 4 * f + 4, t));
                    if (i < 3)
                        y = gelu (y);
                    x = slice (y, 2, 2, 4 * f);

                    // time: same recipe in [1, C, 1, L]
                    const int tco = i < 3 ? kChan[3 - i] : 8;
                    const int lIn = kTimeLen[4 - i], lOut = kTimeLen[3 - i];

                    MPSGraphTensor* yt = add (xt, savedt[3 - i]);
                    MPSGraphTensor* trw = constant (packT3 (m.tdecoder_rewrite_weight[i]), shape4 (2 * ci, ci, 1, 3));
                    MPSGraphTensor* trb = constant (packT1 (m.tdecoder_rewrite_bias[i]), shape4 (1, 2 * ci, 1, 1));
                    yt = gluChannels (conv (yt, trw, trb, 1, 1, 0, 0, 1, 1), ci);

                    yt = dconv (yt, dconvConsts (m, 1, 1, 3 - i), ci, lIn);

                    MPSGraphTensor* ttw = constant (packT3 (m.tdecoder_conv_tr_weight[i]), shape4 (ci, tco, 1, 8));
                    MPSGraphTensor* ttb = constant (packT1 (m.tdecoder_conv_tr_bias[i]), shape4 (1, tco, 1, 1));
                    yt = convT (yt, ttw, ttb, 1, 4, shape4 (1, tco, 1, 4 * lIn + 4));
                    if (i < 3)
                        yt = gelu (yt);
                    xt = slice (yt, 3, 2, lOut);
                }

                specOut = x;   // [1, 16, 2048, 336]
                waveOut = xt;  // [1, 8, 1, 343980]
                tap ("specOut", specOut);
                tap ("waveOut", waveOut);

                // ---- I/O plumbing + one-time compile ---------------------
                specBytes = (size_t) 4 * kBins * kFrames * sizeof (float);
                waveBytes = (size_t) 2 * kSeg * sizeof (float);
                specBuf = [device newBufferWithLength: specBytes options: MTLResourceStorageModeShared];
                waveBuf = [device newBufferWithLength: waveBytes options: MTLResourceStorageModeShared];
                specTD = [[MPSGraphTensorData alloc] initWithMTLBuffer: specBuf
                                                                 shape: shape4 (1, 4, kBins, kFrames)
                                                              dataType: MPSDataTypeFloat32];
                waveTD = [[MPSGraphTensorData alloc] initWithMTLBuffer: waveBuf
                                                                 shape: shape4 (1, 2, 1, kSeg)
                                                              dataType: MPSDataTypeFloat32];

                MPSGraphShapedType* specST = [[MPSGraphShapedType alloc] initWithShape: shape4 (1, 4, kBins, kFrames)
                                                                              dataType: MPSDataTypeFloat32];
                MPSGraphShapedType* waveST = [[MPSGraphShapedType alloc] initWithShape: shape4 (1, 2, 1, kSeg)
                                                                              dataType: MPSDataTypeFloat32];
                exe = [graph compileWithDevice: [MPSGraphDevice deviceWithMTLDevice: device]
                                         feeds: @{ specIn: specST, waveIn: waveST }
                                 targetTensors: @[specOut, waveOut]
                              targetOperations: nil
                        compilationDescriptor: nil];
                if (exe == nil)
                    buildErr = @"MPSGraph compile returned nil";
            }
            @catch (NSException* e)
            {
                exe = nil;
                buildErr = [NSString stringWithFormat: @"MPSGraph build failed: %@", e.reason];
            }
        }
    }
};

// ---- CPU-side segment driver (ports of model_apply.cpp/model_inference.cpp)

void reflectPadding (Eigen::MatrixXf& padded, const Eigen::MatrixXf& mix, int left, int right)
{
    const int n = (int) mix.cols();
    padded.block (0, left, 2, n) = mix;

    for (int i = 0; i < left; ++i)
        padded.block (0, left - 1 - i, 2, 1) = mix.block (0, i, 2, 1);

    for (int i = 0; i < right; ++i)
        padded.block (0, n + left + i, 2, 1) = mix.block (0, n - 1 - i, 2, 1);
}

int symmetricZeroPadding (Eigen::MatrixXf& padded, const Eigen::MatrixXf& original, int totalPadding)
{
    const int left = (int) std::floor ((float) totalPadding / 2.0f);
    const int right = totalPadding - left;
    const int n = (int) original.cols();

    padded.block (0, left, 2, n) = original;
    padded.block (0, 0, 2, left).setZero();
    padded.block (0, n + left, 2, right).setZero();
    return left;
}

// Everything one padded segment needs, allocated once per inference run.
struct SegmentScratch
{
    demucscpp::stft_buffers stft { kPadded };
    Eigen::MatrixXf mix { 2, kSeg };
    Eigen::MatrixXf paddedMix { 2, kPadded };
    std::vector<float> spec = std::vector<float> ((size_t) 4 * kBins * kFrames);
    std::vector<float> wave = std::vector<float> ((size_t) 2 * kSeg);
    std::vector<float> specOut = std::vector<float> ((size_t) 16 * kBins * kFrames);
    std::vector<float> waveOut = std::vector<float> ((size_t) 8 * kSeg);
    Eigen::Tensor3dXf targets { 4, 2, kSeg };
};

// model_inference's CPU shell around the GPU forward pass: STFT + CaC packing
// and per-branch normalisation in, de-normalisation + iSTFT + branch sum out.
bool runModelSegment (MetalHtDemucs& engine, SegmentScratch& s, std::string& err)
{
    reflectPadding (s.paddedMix, s.mix, kPad, kPadEnd);
    s.stft.waveform = s.paddedMix;
    demucscpp::stft (s.stft);

    // CaC pack: channels are [L.re, L.im, R.re, R.im]; frames 2..337 of the
    // padded STFT; the 2049th bin is dropped.
    for (int i = 0; i < 2; ++i)
        for (int f = 0; f < kBins; ++f)
            for (int t = 0; t < kFrames; ++t)
            {
                const auto v = s.stft.spec (i, f, t + 2);
                s.spec[(size_t) (((2 * i + 0) * kBins + f) * kFrames + t)] = v.real();
                s.spec[(size_t) (((2 * i + 1) * kBins + f) * kFrames + t)] = v.imag();
            }

    // (x - mean) / (std + 1e-5) with the reference's N-1 variance
    auto normalise = [] (std::vector<float>& v, float& outMean, float& outStd)
    {
        double sum = 0.0;
        for (auto x : v)
            sum += x;
        const double mean = sum / (double) v.size();

        double sq = 0.0;
        for (auto x : v)
            sq += ((double) x - mean) * ((double) x - mean);
        const auto sd = std::sqrt (sq / (double) (v.size() - 1));

        outMean = (float) mean;
        outStd = (float) sd;
        const auto scale = 1.0f / ((float) sd + kEps);
        for (auto& x : v)
            x = (x - outMean) * scale;
    };

    float mean = 0.0f, std_ = 0.0f, meant = 0.0f, stdt = 0.0f;
    normalise (s.spec, mean, std_);

    for (int ch = 0; ch < 2; ++ch)
        for (int k = 0; k < kSeg; ++k)
            s.wave[(size_t) (ch * kSeg + k)] = s.mix (ch, k);
    normalise (s.wave, meant, stdt);

    if (! engine.run (s.spec.data(), s.wave.data(), s.specOut.data(), s.waveOut.data(), err))
        return false;

    // Per source: undo CaC + normalisation, iSTFT, drop the reflect pad, and
    // add the (de-normalised) time-branch waveform.
    for (int src = 0; src < 4; ++src)
    {
        s.stft.spec.setZero();
        for (int i = 0; i < 2; ++i)
            for (int f = 0; f < kBins; ++f)
                for (int t = 0; t < kFrames; ++t)
                {
                    const auto re = std_ * s.specOut[(size_t) (((src * 4 + 2 * i + 0) * kBins + f) * kFrames + t)] + mean;
                    const auto im = std_ * s.specOut[(size_t) (((src * 4 + 2 * i + 1) * kBins + f) * kFrames + t)] + mean;
                    s.stft.spec (i, f, t + 2) = std::complex<float> (re, im);
                }

        demucscpp::istft (s.stft);

        for (int ch = 0; ch < 2; ++ch)
            for (int k = 0; k < kSeg; ++k)
                s.targets (src, ch, k) = s.stft.waveform (ch, k + kPad)
                                         + (stdt * s.waveOut[(size_t) ((src * 2 + ch) * kSeg + k)] + meant);
    }

    return true;
}

// split_inference: 7.8 s windows on a 75% stride, triangle-weighted
// overlap-add. cb fires once per segment and may throw (cancellation) —
// keep those calls outside every Metal-error guard.
bool splitInference (MetalHtDemucs& engine, SegmentScratch& s, const Eigen::MatrixXf& fullAudio,
                     Eigen::Tensor3dXf& out,
                     const std::function<void (float, const std::string&)>& cb, std::string& err)
{
    const int stride = (int) ((1.0f - demucscpp::OVERLAP) * kSeg);
    const int length = (int) fullAudio.cols();

    out = Eigen::Tensor3dXf (4, 2, length);
    out.setZero();

    Eigen::VectorXf weight (kSeg);
    weight.head (kSeg / 2) = Eigen::VectorXf::LinSpaced (kSeg / 2, 1, kSeg / 2);
    weight.tail (kSeg / 2) = weight.head (kSeg / 2).reverse();
    weight /= weight.maxCoeff();
    // TRANSITION_POWER is 1.0, so the reference's pow() is identity.

    Eigen::VectorXf sumWeight (length);
    sumWeight.setZero();

    const int totalChunks = (int) std::ceil ((float) length / (float) stride);
    const float increment = 1.0f / (float) totalChunks;
    float progress = 0.0f;
    int index = 0;

    for (int offset = 0; offset < length; offset += stride, ++index)
    {
        const int chunkLength = std::min (kSeg, length - offset);

        cb (progress, "[metal] segment " + std::to_string (index + 1) + "/" + std::to_string (totalChunks));

        const int leftPad = symmetricZeroPadding (
            s.mix, Eigen::MatrixXf (fullAudio.block (0, offset, 2, chunkLength)), kSeg - chunkLength);

        if (! runModelSegment (engine, s, err))
            return false;

        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 2; ++j)
                for (int k = 0; k < chunkLength && offset + k < length; ++k)
                    out (i, j, offset + k) += weight (k % chunkLength) * s.targets (i, j, k + leftPad);

        for (int k = 0; k < chunkLength && offset + k < length; ++k)
            sumWeight (offset + k) += weight (k % chunkLength);

        progress += increment;
        cb (progress, "[metal] segment " + std::to_string (index + 1) + "/" + std::to_string (totalChunks) + " done");
    }

    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 2; ++j)
            for (int k = 0; k < length; ++k)
                out (i, j, k) /= sumWeight (k);

    return true;
}

// shift_inference: zero-pad by 0.5 s each side, start at the same fixed
// offset as the CPU path (the model_apply.cpp override — see issue #24 on why
// the offset is no longer random), trim afterwards. Keeping the two backends
// on one offset is what makes their outputs directly comparable.
bool shiftInference (MetalHtDemucs& engine, SegmentScratch& s, const Eigen::MatrixXf& fullAudio,
                     Eigen::Tensor3dXf& out,
                     const std::function<void (float, const std::string&)>& cb, std::string& err)
{
    const int maxShift = (int) (demucscpp::MAX_SHIFT_SECS * demucscpp::SUPPORTED_SAMPLE_RATE);
    const int length = (int) fullAudio.cols();

    Eigen::MatrixXf padded (2, length + 2 * maxShift);
    symmetricZeroPadding (padded, fullAudio, 2 * maxShift);

    const int offset = 1337; // == the CPU override's fixed shift
    Eigen::MatrixXf shifted = padded.block (0, offset, 2, length + maxShift - offset);

    Eigen::Tensor3dXf shiftedOut;
    if (! splitInference (engine, s, shifted, shiftedOut, cb, err))
        return false;

    out = shiftedOut.slice (Eigen::array<int, 3> ({ 0, 0, maxShift - offset }),
                            Eigen::array<int, 3> ({ 4, 2, length }));
    return true;
}

} // namespace

namespace splittr
{
namespace metal
{

    bool gpuAvailable()
    {
        static const bool available = []
        {
            if (std::getenv ("SPLITTR_NO_GPU") != nullptr)
                return false;

            if (@available (macOS 14.0, *))
            {
                id<MTLDevice> device = MTLCreateSystemDefaultDevice();
                // Apple7 == M1: guarantees unified memory and the MPSGraph
                // feature set this graph relies on. Intel/AMD GPUs decline.
                return device != nil && [device supportsFamily: MTLGPUFamilyApple7];
            }

            return false;
        }();

        return available;
    }

    bool inference (const demucscpp::demucs_model& model,
                    const float* interleavedStereo,
                    int numSamples,
                    float* outSources,
                    const std::function<void (float, const std::string&)>& cb,
                    std::string& error)
    {
        if (! gpuAvailable())
        {
            error = "Metal GPU unavailable";
            return false;
        }

        if (! model.is_4sources)
        {
            // The graph is built for the 4-source topology; htdemucs_6s stays
            // on the CPU path rather than risking a wrong-shape compile.
            error = "6-source model not supported on the Metal path";
            return false;
        }

        try
        {
            MetalHtDemucs engine (model);
            if (! engine.valid())
            {
                error = engine.buildError() != nil ? engine.buildError().UTF8String : "Metal engine build failed";
                return false;
            }

            auto scratch = std::make_unique<SegmentScratch>();

            Eigen::Map<const Eigen::MatrixXf> audio (interleavedStereo, 2, numSamples);

            // demucs_inference's mono-reference normalisation, verbatim.
            Eigen::VectorXf refMean0 = audio.colwise().mean();
            const float refMean = refMean0.mean();
            const float refStd = std::sqrt ((refMean0.array() - refMean).square().sum() / (float) (refMean0.size() - 1));

            Eigen::MatrixXf normalised = (audio.array() - refMean) / refStd;

            Eigen::Tensor3dXf result;
            if (! shiftInference (engine, *scratch, normalised, result, cb, error))
                return false;

            Eigen::TensorMap<Eigen::Tensor3dXf> out (outSources, 4, 2, numSamples);
            out = (result * refStd).eval() + refMean;
            return true;
        }
        catch (const std::bad_alloc&)
        {
            error = "out of memory on the Metal path";
            return false;
        }
        // Anything else — including the callback's cancellation exception —
        // belongs to the caller.
    }

    bool debugSegmentTaps (const demucscpp::demucs_model& model,
                           const float* spec,
                           const float* wave,
                           const std::vector<std::string>& tapNames,
                           std::vector<std::vector<float>>& tapData,
                           std::string& error)
    {
        if (! gpuAvailable())
        {
            error = "Metal GPU unavailable";
            return false;
        }

        MetalHtDemucs engine (model);
        if (! engine.valid())
        {
            error = engine.buildError() != nil ? engine.buildError().UTF8String : "Metal engine build failed";
            return false;
        }

        return engine.runTaps (spec, wave, tapNames, tapData, error);
    }

} // namespace metal
} // namespace splittr

#pragma clang diagnostic pop

#endif // __APPLE__
