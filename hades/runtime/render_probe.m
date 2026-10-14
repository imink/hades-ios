#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <CommonCrypto/CommonDigest.h>
#include "../../dm/gfx/nvn/nvn_spirv_bind.h"
#include "probes.h"

static void require(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "[hades-render] %s\n", message);
        exit(2);
    }
}

static NSString *digest(NSData *data) {
    unsigned char bytes[32];
    CC_SHA256(data.bytes, (CC_LONG)data.length, bytes);
    NSMutableString *text = [NSMutableString string];
    for (unsigned i = 0; i < 32; i++)
        [text appendFormat:@"%02x", bytes[i]];
    return text;
}

typedef struct {
    __unsafe_unretained id<MTLRenderCommandEncoder> encoder;
    __unsafe_unretained id<MTLBuffer> uniform;
    __unsafe_unretained id<MTLTexture> texture;
    __unsafe_unretained id<MTLSamplerState> sampler;
} resources;

static int encode_binding(void *pointer, const nvn_spirv_binding *binding, uint64_t value, uint64_t size) {
    resources *resource = pointer;
    if (binding->kind == NVN_SPIRV_BUFFER) {
        uintptr_t base = (uintptr_t)resource->uniform.contents;
        if (binding->stage || value < base || value - base > resource->uniform.length
            || size > resource->uniform.length - (value - base))
            return -1;
        [resource->encoder setVertexBuffer:resource->uniform offset:(NSUInteger)(value - base)
                                  atIndex:binding->msl_index];
    } else if (binding->kind == NVN_SPIRV_TEXTURE) {
        if (binding->stage != 1 || value != 42 || !resource->texture)
            return -1;
        [resource->encoder setFragmentTexture:resource->texture atIndex:binding->msl_index];
    } else if (binding->kind == NVN_SPIRV_SAMPLER) {
        if (binding->stage != 1 || value != 5 || !resource->sampler)
            return -1;
        [resource->encoder setFragmentSamplerState:resource->sampler atIndex:binding->msl_index];
    } else {
        return -1;
    }
    return 0;
}

#ifndef HADES_IOS_APP
int main(int argc, char **argv) {
    return hades_run_render_probe(argc, (const char *const *)argv);
}
#endif

static NSDictionary *shader(NSArray *catalog, NSString *name) {
    for (NSDictionary *record in catalog)
        if ([record[@"file"] isEqual:name])
            return record;
    require(0, "Required test shader is not in the verified catalog");
    return nil;
}

static id<MTLFunction> function(id<MTLDevice> device, NSString *directory, NSDictionary *record) {
    NSString *path = [directory stringByAppendingPathComponent:[record[@"file"] stringByAppendingString:@".metal"]];
    NSData *data = [NSData dataWithContentsOfFile:path];
    require(data && [digest(data) isEqual:record[@"msl_sha256"]], "MSL differs from the verified interface catalog");
    NSString *source = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
    require(source != nil, "Invalid generated MSL text");
    NSError *error = nil;
    MTLCompileOptions *options = [MTLCompileOptions new];
    options.languageVersion = MTLLanguageVersion2_1;
    id<MTLLibrary> library = [device newLibraryWithSource:source options:options error:&error];
    if (!library) {
        fprintf(stderr, "[hades-render] OS Metal runtime compilation failed: %s\n", error.localizedDescription.UTF8String);
        exit(2);
    }
    id<MTLFunction> result = [library newFunctionWithName:record[@"entry"]];
    require(result != nil, "Translated shader entry is missing");
    return result;
}

typedef struct { float position[4], color[4], uv[2], origin[3]; } vertex;
_Static_assert(sizeof(vertex) == 52, "Test vertex packing");

int hades_run_render_probe(int argc, const char *const *argv) {
    @autoreleasepool {
        require(argc == 3, "Usage: render-probe <verified MSL directory> <bindings.json>");
        NSString *directory = @(argv[1]);
        NSError *error = nil;
        NSData *data = [NSData dataWithContentsOfFile:@(argv[2])];
        NSDictionary *catalog = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:&error] : nil;
        require([catalog[@"status"] isEqual:@"direct-msl-interface-verified"], "Missing verified binding catalog");
        NSDictionary *vs = shader(catalog[@"shaders"], @"Default2D.vert");
        NSDictionary *fs = shader(catalog[@"shaders"], @"Default.frag");
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        require(device != nil, "No local Metal GPU device");
        NSMutableDictionary<NSString *, id<MTLFunction>> *functions = [NSMutableDictionary dictionary];
        for (NSDictionary *record in catalog[@"shaders"]) {
            id<MTLFunction> compiled = function(device, directory, record);
            MTLFunctionType expected = [record[@"stage"] isEqual:@"vertex"] ? MTLFunctionTypeVertex : MTLFunctionTypeFragment;
            require(compiled.functionType == expected, "Compiled shader stage differs from the verified interface");
            functions[record[@"file"]] = compiled;
        }
        printf("MSL_COMPILE_OK stages=%lu\n", (unsigned long)functions.count);
        MTLRenderPipelineDescriptor *pipeline = [MTLRenderPipelineDescriptor new];
        pipeline.vertexFunction = functions[vs[@"file"]];
        pipeline.fragmentFunction = functions[fs[@"file"]];
        pipeline.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        MTLVertexDescriptor *attributes = [MTLVertexDescriptor vertexDescriptor];
        const MTLVertexFormat formats[] = {MTLVertexFormatFloat4, MTLVertexFormatFloat4,
                                          MTLVertexFormatFloat2, MTLVertexFormatFloat3};
        const NSUInteger offsets[] = {0, 16, 32, 40};
        for (unsigned i = 0; i < 4; i++) {
            attributes.attributes[i].format = formats[i];
            attributes.attributes[i].offset = offsets[i];
            attributes.attributes[i].bufferIndex = 16;
        }
        attributes.layouts[16].stride = sizeof(vertex);
        pipeline.vertexDescriptor = attributes;
        id<MTLRenderPipelineState> state = [device newRenderPipelineStateWithDescriptor:pipeline error:&error];
        if (!state) {
            fprintf(stderr, "[hades-render] Pipeline failed: %s\n", error.localizedDescription.UTF8String);
            return 2;
        }
        const vertex triangle[] = {
            {{-1,-1,0,0},{1,1,1,1},{0.5,0.5},{0,0,1}},
            {{ 3,-1,0,0},{1,1,1,1},{0.5,0.5},{0,0,1}},
            {{-1, 3,0,0},{1,1,1,1},{0.5,0.5},{0,0,1}}};
        id<MTLBuffer> vertices = [device newBufferWithBytes:triangle length:sizeof triangle options:MTLResourceStorageModeShared];
        id<MTLBuffer> uniform = [device newBufferWithLength:32 options:MTLResourceStorageModeShared];
        MTLTextureDescriptor *description = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                            width:1 height:1 mipmapped:NO];
        description.storageMode = MTLStorageModeShared;
        description.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> texture = [device newTextureWithDescriptor:description];
        MTLSamplerDescriptor *sampling = [MTLSamplerDescriptor new];
        sampling.minFilter = MTLSamplerMinMagFilterNearest;
        sampling.magFilter = MTLSamplerMinMagFilterNearest;
        id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:sampling];
        description = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:32 height:32 mipmapped:NO];
        description.storageMode = MTLStorageModeShared;
        description.usage = MTLTextureUsageRenderTarget;
        id<MTLTexture> target = [device newTextureWithDescriptor:description];
        id<MTLCommandQueue> queue = [device newCommandQueue];
        require(vertices && uniform && texture && sampler && target && queue, "Cannot allocate Metal probe resources");
        nvn_spirv_binding plan[3];
        unsigned count = 0;
        for (unsigned stage = 0; stage < 2; stage++) {
            NSDictionary *record = stage ? fs : vs;
            for (NSDictionary *binding in record[@"resources"]) {
                if ([binding[@"msl_index"] isKindOfClass:[NSNull class]])
                    continue;
                NSString *kind = binding[@"kind"];
                unsigned type = [kind isEqual:@"buffer"] ? NVN_SPIRV_BUFFER :
                                [kind isEqual:@"texture"] ? NVN_SPIRV_TEXTURE : NVN_SPIRV_SAMPLER;
                require(count < 3 && ([kind isEqual:@"buffer"] || [kind isEqual:@"texture"] || [kind isEqual:@"sampler"]),
                        "Unexpected direct MSL resource");
                /* This explicit test-client NVN map is not the unobserved guest GL program map. */
                plan[count++] = (nvn_spirv_binding){type, stage, [binding[@"msl_index"] unsignedIntValue],
                                                   0, [binding[@"block_size"] unsignedLongLongValue]};
            }
        }
        require(count == 3, "Unexpected test shader interface");
        nvn_spirv_state bindings = {0};
        char diagnostic[160];
        p_bind_buf buffer = {0, 0, (uintptr_t)uniform.contents, 32};
        p_bind_handles textures = {.stage=1, .first=0, .n=1, .h={42}};
        p_bind_handles samplers = {.stage=1, .first=0, .n=1, .h={5ull << 20}};
        require(!nvn_spirv_record(&bindings, OP_BIND_UBO, &buffer, sizeof buffer, diagnostic, sizeof diagnostic)
                && !nvn_spirv_record(&bindings, OP_BIND_TEX, &textures, sizeof textures, diagnostic, sizeof diagnostic)
                && !nvn_spirv_record(&bindings, OP_BIND_SAMP, &samplers, sizeof samplers, diagnostic, sizeof diagnostic),
                "Cannot replay NVN binding commands");
        for (unsigned frame = 0; frame < 3; frame++) {
            float scale = frame == 1 ? 0.5f : 1;
            float projection[] = {scale,scale,0,0,scale,scale,0,0};
            memcpy(uniform.contents, projection, sizeof projection);
            const unsigned char color[4] = {frame == 2 ? 32 : 64, frame == 2 ? 96 : 128,
                                           frame == 2 ? 224 : 192, 255};
            [texture replaceRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0 withBytes:color bytesPerRow:4];
            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = target;
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            pass.colorAttachments[0].clearColor = MTLClearColorMake(0,0,0,0);
            id<MTLCommandBuffer> command = [queue commandBuffer];
            id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
            require(command && encoder, "Cannot create Metal render submission");
            [encoder setRenderPipelineState:state];
            [encoder setVertexBuffer:vertices offset:0 atIndex:16];
            resources objects = {encoder, uniform, texture, sampler};
            if (nvn_spirv_apply(&bindings, plan, count, encode_binding, &objects, diagnostic, sizeof diagnostic)) {
                fprintf(stderr, "[hades-render] %s\n", diagnostic);
                return 2;
            }
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [encoder endEncoding];
            [command commit];
            [command waitUntilCompleted];
            require(command.status == MTLCommandBufferStatusCompleted, "Metal submission failed");
            unsigned char pixels[32 * 32 * 4];
            [target getBytes:pixels bytesPerRow:128 fromRegion:MTLRegionMake2D(0,0,32,32) mipmapLevel:0];
            unsigned colored = 0;
            for (unsigned pixel = 0; pixel < 1024; pixel++) {
                const unsigned char *actual = pixels + pixel * 4;
                if (!memcmp(actual, color, 4))
                    colored++;
                else
                    require(!actual[0] && !actual[1] && !actual[2] && !actual[3], "Unexpected GPU pixel");
            }
            require(frame == 1 ? colored > 0 && colored < 1024 : colored == 1024,
                    "Uniform projection or sampled texture binding did not affect the rendered frame");
            printf("FRAME_OK %u colored=%u total=1024\n", frame, colored);
        }
        printf("RENDER_OK: actual Hades shader pair, NVN binding payloads, Metal submission/readback; no guest program or onscreen present\n");
    }
    return 0;
}
