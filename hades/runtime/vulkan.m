#define VK_USE_PLATFORM_METAL_EXT
#define VK_USE_PLATFORM_VI_NN
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <vulkan/vulkan.h>
#include <MoltenVK/mvk_deprecated_api.h>
#include "vulkan.h"
#include "host.h"
#include "memory.h"
#include "ipc.h"
#include "diagnostics.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <limits.h>
#include <fcntl.h>
#include <unistd.h>

static VkInstance bootstrap;
static CAMetalLayer *surface_layer;
static id<MTLDevice> metal_device;
static int graphics_ready, vi_ready;
static int queue_completion_probe;
static unsigned char display_token, layer_token, window_token;
static pthread_mutex_t resource_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { VkDeviceMemory memory; VkDevice device; VkDeviceSize size;
                const VkAllocationCallbacks *allocator; int mapped; } allocations[1024];
static _Atomic unsigned long presents;
uint64_t hades_vulkan_present_count(void) {
    return atomic_load_explicit(&presents,memory_order_relaxed);
}
static char capture_directory[PATH_MAX];
static unsigned capture_step;
static _Atomic unsigned long capture_presents;
static pthread_mutex_t capture_lock=PTHREAD_MUTEX_INITIALIZER;
static struct { VkDevice device; VkSwapchainKHR swapchain; } capture_swapchains[32];
enum { CAPTURE_IMAGE_LIMIT=2048 };
static struct { VkImage image; VkFormat format; VkExtent3D extent;
                VkImageUsageFlags usage; unsigned long serial; } capture_images[CAPTURE_IMAGE_LIMIT];
static unsigned long capture_image_serial;
static _Atomic unsigned capture_image_overflows;
static int attachment_load_probe;
static unsigned long pass_capture_frame;
static VkPhysicalDevice pass_physical;
static struct { VkImageView view; VkImageViewCreateInfo info; } pass_views[2048];
static struct { VkRenderPass pass; VkAttachmentDescription attachments[16]; unsigned count; } pass_passes[512];
static struct { VkFramebuffer framebuffer; VkDevice device; VkRenderPass pass;
                VkImageView views[16]; unsigned count,width,height,layers; } pass_frames[512];
static struct { VkCommandBuffer command; VkFramebuffer framebuffer; VkRenderPass pass; unsigned draws; } pass_commands[128];
enum { PASS_READ_LIMIT=96 };
static struct { VkCommandBuffer command; VkDevice device; VkBuffer buffer; VkDeviceMemory memory;
                unsigned width,height,serial; VkFormat format; } pass_reads[PASS_READ_LIMIT];
static unsigned pass_read_count;
static VkDeviceSize pass_read_bytes;
static struct { VkShaderModule module; uint64_t fingerprint; } pass_shaders[512];
static int vk_error(const char *message);

static unsigned preserve_color_loads(VkAttachmentDescription *attachments,unsigned count) {
    unsigned changed=0;
    for (unsigned i=0;i<count;i++)
        if (attachments[i].loadOp==VK_ATTACHMENT_LOAD_OP_DONT_CARE
            && attachments[i].initialLayout==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
            && attachments[i].finalLayout==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
            attachments[i].loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;
            changed++;
        }
    return changed;
}

int hades_vulkan_configure_capture(const char *directory,unsigned step) {
    struct stat info;
    if (bootstrap || capture_step || !directory || directory[0]!='/' || !step || step>10000
        || strlen(directory)>=sizeof capture_directory || lstat(directory,&info) || !S_ISDIR(info.st_mode)) {
        fprintf(stderr,"[hades-capture] Invalid explicit private capture directory/interval\n");
        return -1;
    }
    strlcpy(capture_directory,directory,sizeof capture_directory);
    capture_step=step;
    fprintf(stderr,"[hades-capture] Explicit own-texture readback enabled; adds swapchain TRANSFER_SRC, never desktop capture\n");
    return 0;
}

static int write_pixels(const uint8_t *source,NSUInteger width,NSUInteger height,NSUInteger row,
    BOOL bgra,const char *path,BOOL capture_alpha) {
    int descriptor=open(path,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
    FILE *file=descriptor<0 ? NULL : fdopen(descriptor,"wb");
    if (!file && descriptor>=0 && close(descriptor))
        fprintf(stderr,"[hades-capture] Cannot close uninitialized capture descriptor\n");
    FILE *alpha=NULL;
    if (file && capture_alpha) {
        char alpha_path[PATH_MAX];
        int length=snprintf(alpha_path,sizeof alpha_path,"%s.alpha.pgm",path);
        int alpha_descriptor=length<0 || length>=(int)sizeof alpha_path ? -1
            : open(alpha_path,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
        alpha=alpha_descriptor<0 ? NULL : fdopen(alpha_descriptor,"wb");
        if (!alpha && alpha_descriptor>=0 && close(alpha_descriptor))
            fprintf(stderr,"[hades-capture] Cannot close uninitialized alpha descriptor\n");
    }
    uint8_t *rgb=malloc(width*3);
    if (!file || !rgb || (capture_alpha && !alpha)) {
        if (file && fclose(file))
            fprintf(stderr,"[hades-capture] Cannot close incomplete capture\n");
        if (alpha && fclose(alpha))
            fprintf(stderr,"[hades-capture] Cannot close incomplete alpha capture\n");
        free(rgb);
        fprintf(stderr,"[hades-capture] Cannot exclusively create private capture file/row\n");
        return -1;
    }
    int failed=fprintf(file,"P6\n%lu %lu\n255\n",(unsigned long)width,(unsigned long)height)<0;
    if (alpha && fprintf(alpha,"P5\n%lu %lu\n255\n",(unsigned long)width,(unsigned long)height)<0)
        failed=1;
    for (NSUInteger y=0;y<height && !failed;y++) {
        for (NSUInteger x=0;x<width;x++) {
            const uint8_t *pixel=source+y*row+x*4;
            rgb[x*3]=pixel[bgra ? 2 : 0];rgb[x*3+1]=pixel[1];rgb[x*3+2]=pixel[bgra ? 0 : 2];
        }
        failed=fwrite(rgb,3,width,file)!=width;
        if (alpha && !failed) {
            for (NSUInteger x=0;x<width;x++)
                rgb[x]=source[y*row+x*4+3];
            failed=fwrite(rgb,1,width,alpha)!=width;
        }
    }
    if (fclose(file))
        failed=1;
    if (alpha && fclose(alpha))
        failed=1;
    free(rgb);
    fprintf(stderr,"[hades-capture] OWN_GPU_TEXTURE_READBACK extent=%lux%lu status=%s; no inferred pixel correctness\n",
            (unsigned long)width,(unsigned long)height,failed ? "failed" : "written");
    return failed ? -1 : 0;
}

static int write_texture(id<MTLTexture> texture,const char *path,BOOL capture_alpha) {
    NSUInteger width=texture.width,height=texture.height;
    BOOL bgra=texture.pixelFormat==MTLPixelFormatBGRA8Unorm || texture.pixelFormat==MTLPixelFormatBGRA8Unorm_sRGB;
    BOOL rgba=texture.pixelFormat==MTLPixelFormatRGBA8Unorm || texture.pixelFormat==MTLPixelFormatRGBA8Unorm_sRGB;
    if (!texture || texture.framebufferOnly || (!bgra && !rgba) || !width || !height
        || width>8192 || height>8192) {
        fprintf(stderr,"[hades-capture] Unsupported/non-readable owned texture format or extent\n");
        return -1;
    }
    NSUInteger row=(width*4+255)&~(NSUInteger)255;
    id<MTLBuffer> buffer=[texture.device newBufferWithLength:row*height options:MTLResourceStorageModeShared];
    id<MTLCommandQueue> queue=[texture.device newCommandQueue];
    id<MTLCommandBuffer> command=[queue commandBuffer];
    id<MTLBlitCommandEncoder> blit=[command blitCommandEncoder];
    if (!buffer || !queue || !command || !blit) {
        fprintf(stderr,"[hades-capture] Cannot allocate actual texture readback resources\n");
        return -1;
    }
    [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
        sourceSize:MTLSizeMake(width,height,1) toBuffer:buffer destinationOffset:0
        destinationBytesPerRow:row destinationBytesPerImage:row*height];
    [blit endEncoding];
    [command commit];[command waitUntilCompleted];
    if (command.status!=MTLCommandBufferStatusCompleted) {
        fprintf(stderr,"[hades-capture] GPU readback failed: %s\n",command.error.localizedDescription.UTF8String);
        return -1;
    }
    return write_pixels(buffer.contents,width,height,row,bgra,path,capture_alpha);
}

static int pass_counts_selected(unsigned long completed,unsigned long entered) {
    return pass_capture_frame && capture_step
        && completed+1==pass_capture_frame && entered<pass_capture_frame;
}

static int pass_frame_selected(void) {
    return pass_counts_selected(atomic_load_explicit(&presents,memory_order_relaxed),
        atomic_load_explicit(&capture_presents,memory_order_relaxed));
}

static unsigned long flow_counts_selected(unsigned long completed,unsigned long entered) {
    unsigned long next=completed+1;
    return pass_capture_frame && entered<next
        && (next==pass_capture_frame || next==pass_capture_frame*2) ? next : 0;
}

static unsigned flow_static_load(uintptr_t pc,uint32_t page,uint32_t load,uintptr_t *address) {
    if (pc%4 || pc>=(1ull<<39) || (page&0x9f000000)!=0x90000000
        || (load&0x3fc00000)!=0x39400000 || (page&31)==31
        || ((load>>5)&31)!=(page&31))
        return 0;
    int64_t pages=((page>>5)&0x7ffff)*4+((page>>29)&3);
    if (pages&(1<<20))
        pages-=1<<21;
    unsigned width=1u<<(load>>30);
    int64_t target=(int64_t)(pc&~(uintptr_t)4095)+pages*4096
        +((load>>10)&4095)*width;
    if (target<0 || (uint64_t)target+width>(1ull<<39))
        return 0;
    *address=(uintptr_t)target;
    return width;
}

static unsigned trace_flow_static_loads(unsigned long frame,uintptr_t caller) {
    hades_memory_info code;
    if (caller%4 || hades_memory_info_query(caller,&code) || code.state!=3
        || (code.permissions&5)!=5)
        return 0;
    size_t available=code.size-(caller-code.base);
    size_t bytes=available<0x240 ? available : 0x240;
    unsigned samples=0;
    for (size_t offset=0;offset+8<=bytes;offset+=4) {
        uint32_t instructions[2];
        memcpy(instructions,(const void *)(caller+offset),sizeof instructions);
        uintptr_t address;
        unsigned width=flow_static_load(caller+offset,instructions[0],instructions[1],&address);
        hades_memory_info data;
        if (!width || hades_memory_info_query(address,&data) || data.state!=4
            || !(data.permissions&1) || data.size-(address-data.base)<width)
            continue;
        if (samples==8) {
            fprintf(stderr,"[hades-capture] PASS_FLOW_STATIC_LIMIT frame=%lu caller=%#llx samples=8; further nearby loads not sampled\n",
                frame,(unsigned long long)caller);
            break;
        }
        uint64_t value=0;
        memcpy(&value,(const void *)address,width);
        fprintf(stderr,"[hades-capture] PASS_FLOW_STATIC_LOAD frame=%lu caller=%#llx instruction=%#llx address=%#llx width=%u value=%#llx; nearby decoded load, not proof of execution\n",
            frame,(unsigned long long)caller,(unsigned long long)(caller+offset),
            (unsigned long long)address,width,(unsigned long long)value);
        samples++;
    }
    return samples;
}

static int trace_flow_forge_batch(unsigned long frame,const uint64_t *r) {
    uintptr_t caller=(uintptr_t)r[30],record=(uintptr_t)r[19];
    hades_memory_info code,data;
    if (caller%4 || hades_memory_info_query(caller,&code) || code.state!=3
        || (code.permissions&5)!=5 || caller-code.base<0x90
        || hades_memory_info_query(record,&data) || data.state!=4
        || !(data.permissions&1) || data.size-(record-data.base)<0x960)
        return 0;
    uint32_t fingerprint[4],fields[4];
    memcpy(fingerprint,(const void *)(caller-0x90),sizeof fingerprint);
    if (fingerprint[0]!=0x9b082553 || (fingerprint[1]&0x9f00001f)!=0x90000009
        || fingerprint[2]!=0xf9441529 || fingerprint[3]!=0xb9495e68)
        return 0;
    memcpy(&fields[0],(const void *)(record+8),4);
    memcpy(&fields[1],(const void *)(record+0x10),4);
    memcpy(&fields[2],(const void *)(record+0x958),8);
    fprintf(stderr,"[hades-capture] PASS_FLOW_FORGE_BATCH frame=%lu caller=%#llx record=%#llx descriptor-index=%u descriptor-capacity=%u vertex-stride=%u vertex-layout=%u; verified nearby instruction pattern, not proof of rejection\n",
        frame,(unsigned long long)caller,(unsigned long long)record,
        fields[0],fields[1],fields[2],fields[3]);
    return 1;
}

static void trace_pass_flow(unsigned index,const char *name,const uint64_t *r) {
    unsigned long frame=flow_counts_selected(atomic_load_explicit(&presents,memory_order_relaxed),
        atomic_load_explicit(&capture_presents,memory_order_relaxed));
    if (!frame)
        return;
    static _Atomic unsigned calls;
    unsigned number=atomic_fetch_add_explicit(&calls,1,memory_order_relaxed);
    if (number>=16384) {
        if (number==16384)
            fprintf(stderr,"[hades-capture] PASS_FLOW_LIMIT calls=16384; further CPU calls not observed\n");
        return;
    }
    fprintf(stderr,"[hades-capture] PASS_FLOW_CALL frame=%lu seq=%u api=%s r0=%#llx r1=%#llx r2=%#llx r3=%#llx r4=%#llx r5=%#llx r6=%#llx r7=%#llx lr=%#llx fpcr=%#llx\n",
        frame,number+1,name,(unsigned long long)r[0],(unsigned long long)r[1],
        (unsigned long long)r[2],(unsigned long long)r[3],(unsigned long long)r[4],
        (unsigned long long)r[5],(unsigned long long)r[6],(unsigned long long)r[7],
        (unsigned long long)r[30],(unsigned long long)r[98]);
    if (index==HADES_HOST_VULKAN_BIND_PIPELINE) {
        trace_flow_static_loads(frame,(uintptr_t)r[30]);
    } else if (index==HADES_HOST_VULKAN_BIND_VERTEX_BUFFERS) {
        trace_flow_forge_batch(frame,r);
    } else if (index==HADES_HOST_VULKAN_BEGIN_RENDER_PASS) {
        const VkRenderPassBeginInfo *info=(const void *)(uintptr_t)r[1];
        fprintf(stderr,"[hades-capture] PASS_FLOW_BEGIN frame=%lu command=%#llx framebuffer=%#llx pass=%#llx area=%d/%d/%u/%u clear-values=%u\n",
            frame,(unsigned long long)r[0],(unsigned long long)info->framebuffer,
            (unsigned long long)info->renderPass,info->renderArea.offset.x,info->renderArea.offset.y,
            info->renderArea.extent.width,info->renderArea.extent.height,info->clearValueCount);
    } else if (index==HADES_HOST_VULKAN_SCISSOR) {
        const VkRect2D *rectangles=(const void *)(uintptr_t)r[3];
        for (unsigned i=0;i<r[2] && i<4;i++)
            fprintf(stderr,"[hades-capture] PASS_FLOW_SCISSOR frame=%lu command=%#llx index=%u rectangle=%d/%d/%u/%u\n",
                frame,(unsigned long long)r[0],(unsigned)r[1]+i,
                rectangles[i].offset.x,rectangles[i].offset.y,rectangles[i].extent.width,rectangles[i].extent.height);
    } else if (index==HADES_HOST_VULKAN_END_RENDER_PASS) {
        fprintf(stderr,"[hades-capture] PASS_FLOW_END_CPU frame=%lu command=%#llx x19=%#llx x20=%#llx x21=%#llx x22=%#llx x23=%#llx x24=%#llx x25=%#llx x26=%#llx x27=%#llx x28=%#llx fp=%#llx lr=%#llx\n",
            frame,(unsigned long long)r[0],(unsigned long long)r[19],(unsigned long long)r[20],
            (unsigned long long)r[21],(unsigned long long)r[22],(unsigned long long)r[23],
            (unsigned long long)r[24],(unsigned long long)r[25],(unsigned long long)r[26],
            (unsigned long long)r[27],(unsigned long long)r[28],(unsigned long long)r[29],
            (unsigned long long)r[30]);
    }
}

static int record_pass_pixels(VkCommandBuffer command,VkFramebuffer framebuffer,VkRenderPass render_pass,unsigned draws) {
    unsigned f,p;
    for (f=0;f<512 && pass_frames[f].framebuffer!=framebuffer;f++);
    if (f==512)
        return vk_error("Pass capture framebuffer is not registered");
    for (p=0;p<512 && pass_passes[p].pass!=render_pass;p++);
    if (p==512)
        return vk_error("Pass capture render pass is not registered");
    if (pass_passes[p].count!=pass_frames[f].count)
        return vk_error("Pass capture framebuffer/pass attachment counts differ");
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(pass_physical,&properties);
    for (unsigned a=0;a<pass_passes[p].count;a++) {
        const VkAttachmentDescription *attachment=&pass_passes[p].attachments[a];
        if ((attachment->format!=VK_FORMAT_R8G8B8A8_UNORM && attachment->format!=VK_FORMAT_B8G8R8A8_UNORM)
            || attachment->samples!=VK_SAMPLE_COUNT_1_BIT) {
            fprintf(stderr,"[hades-capture] PASS_ATTACHMENT_SKIPPED frame=%lu attachment=%u format=%u samples=%u\n",
                pass_capture_frame,a,attachment->format,attachment->samples);
            continue;
        }
        if (pass_read_count==PASS_READ_LIMIT) {
            fprintf(stderr,"[hades-capture] PASS_CAPTURE_LIMIT frame=%lu limit=%u; further attachments not observed\n",
                pass_capture_frame,PASS_READ_LIMIT);
            return 0;
        }
        unsigned v;
        for (v=0;v<2048 && pass_views[v].view!=pass_frames[f].views[a];v++);
        if (v==2048)
            return vk_error("Pass capture image view is not registered");
        if (pass_frames[f].layers!=1 || pass_views[v].info.subresourceRange.layerCount!=1) {
            fprintf(stderr,"[hades-capture] PASS_ATTACHMENT_SKIPPED frame=%lu attachment=%u framebuffer-layers=%u view-layers=%u; layered output not observed\n",
                pass_capture_frame,a,pass_frames[f].layers,pass_views[v].info.subresourceRange.layerCount);
            continue;
        }
        VkDevice device=pass_frames[f].device;
        unsigned width=pass_frames[f].width,height=pass_frames[f].height;
        VkDeviceSize bytes=(VkDeviceSize)width*height*4;
        if (!width || !height || bytes>16*1024*1024 || pass_read_bytes+bytes>384*1024*1024)
            return vk_error("Pass capture exceeds its explicit readback memory budget");
        VkBufferCreateInfo buffer_info={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size=bytes,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
        VkBuffer buffer;
        VkResult result=vkCreateBuffer(device,&buffer_info,NULL,&buffer);
        if (result!=VK_SUCCESS)
            return vk_error("Pass capture buffer allocation failed");
        VkMemoryRequirements requirements;
        vkGetBufferMemoryRequirements(device,buffer,&requirements);
        unsigned type;
        for (type=0;type<properties.memoryTypeCount;type++)
            if ((requirements.memoryTypeBits&(1u<<type))
                && (properties.memoryTypes[type].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
                    ==(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
                break;
        if (type==properties.memoryTypeCount) {
            vkDestroyBuffer(device,buffer,NULL);
            return vk_error("Pass capture has no coherent host-readable memory type");
        }
        VkMemoryAllocateInfo allocation={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize=requirements.size,.memoryTypeIndex=type};
        VkDeviceMemory memory;
        result=vkAllocateMemory(device,&allocation,NULL,&memory);
        if (result!=VK_SUCCESS) {
            vkDestroyBuffer(device,buffer,NULL);
            return vk_error("Pass capture memory allocation failed");
        }
        result=vkBindBufferMemory(device,buffer,memory,0);
        if (result!=VK_SUCCESS) {
            vkDestroyBuffer(device,buffer,NULL);vkFreeMemory(device,memory,NULL);
            return vk_error("Pass capture buffer binding failed");
        }
        VkImageSubresourceRange range=pass_views[v].info.subresourceRange;
        range.levelCount=1;range.layerCount=1;
        VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout=attachment->finalLayout,.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .image=pass_views[v].info.image,.subresourceRange=range};
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,NULL,0,NULL,1,&barrier);
        VkBufferImageCopy copy={.imageSubresource={range.aspectMask,range.baseMipLevel,range.baseArrayLayer,1},
            .imageExtent={width,height,1}};
        vkCmdCopyImageToBuffer(command,barrier.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer,1,&copy);
        barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;barrier.newLayout=attachment->finalLayout;
        barrier.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask=attachment->finalLayout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
            ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,NULL,0,NULL,1,&barrier);
        unsigned slot=pass_read_count++;
        pass_reads[slot].command=command;pass_reads[slot].device=device;
        pass_reads[slot].buffer=buffer;pass_reads[slot].memory=memory;
        pass_reads[slot].width=width;pass_reads[slot].height=height;
        pass_reads[slot].serial=slot+1;pass_reads[slot].format=attachment->format;
        pass_read_bytes+=bytes;
        fprintf(stderr,"[hades-capture] PASS_RECORDED frame=%lu serial=%u command=%#llx framebuffer=%#llx pass=%#llx image=%#llx attachment=%u draws=%u format=%u extent=%ux%u\n",
            pass_capture_frame,slot+1,(unsigned long long)command,(unsigned long long)framebuffer,
            (unsigned long long)render_pass,(unsigned long long)barrier.image,a,draws,attachment->format,width,height);
    }
    return 0;
}

static int flush_pass_pixels(VkQueue queue,uint32_t count,const VkSubmitInfo *submits) {
    for (unsigned i=0;i<pass_read_count;i++) {
        if (!pass_reads[i].buffer)
            continue;
        int submitted=0;
        for (unsigned s=0;s<count;s++)
            for (unsigned c=0;c<submits[s].commandBufferCount;c++)
                submitted|=submits[s].pCommandBuffers[c]==pass_reads[i].command;
        if (!submitted)
            continue;
        VkResult result=vkQueueWaitIdle(queue);
        if (result!=VK_SUCCESS)
            return vk_error("Pass capture submitted queue did not complete");
        void *pixels=NULL;
        result=vkMapMemory(pass_reads[i].device,pass_reads[i].memory,0,VK_WHOLE_SIZE,0,&pixels);
        if (result!=VK_SUCCESS)
            return vk_error("Pass capture readback mapping failed");
        char path[PATH_MAX];
        int length=snprintf(path,sizeof path,"%s/pass-frame-%lu-serial-%u.ppm",
            capture_directory,pass_capture_frame,pass_reads[i].serial);
        int failed=length<0 || length>=(int)sizeof path
            || write_pixels(pixels,pass_reads[i].width,pass_reads[i].height,pass_reads[i].width*4,
                pass_reads[i].format==VK_FORMAT_B8G8R8A8_UNORM,path,YES);
        vkUnmapMemory(pass_reads[i].device,pass_reads[i].memory);
        vkDestroyBuffer(pass_reads[i].device,pass_reads[i].buffer,NULL);
        vkFreeMemory(pass_reads[i].device,pass_reads[i].memory,NULL);
        pass_reads[i].buffer=VK_NULL_HANDLE;
        pass_read_bytes-=(VkDeviceSize)pass_reads[i].width*pass_reads[i].height*4;
        if (failed)
            return vk_error("Pass capture pixel file write failed");
        fprintf(stderr,"[hades-capture] PASS_PIXELS_WRITTEN frame=%lu serial=%u; inserted copy/barriers perturb this diagnostic frame\n",
            pass_capture_frame,pass_reads[i].serial);
    }
    return 0;
}

static int track_pass_call(unsigned index,const uint64_t input[8],const uint64_t *result) {
    if (!pass_capture_frame)
        return 0;
    pthread_mutex_lock(&capture_lock);
    int status=0;
    if (index==HADES_HOST_VULKAN_CREATE_SHADER && result[0]==VK_SUCCESS) {
        const VkShaderModuleCreateInfo *info=(const void *)(uintptr_t)input[1];
        unsigned slot;
        for (slot=0;slot<512 && pass_shaders[slot].module;slot++);
        if (slot==512) status=vk_error("Pass capture shader fingerprint registry exhausted");
        else {
            uint64_t fingerprint=14695981039346656037ull;
            const uint8_t *bytes=(const void *)info->pCode;
            for (size_t i=0;i<info->codeSize;i++) fingerprint=(fingerprint^bytes[i])*1099511628211ull;
            pass_shaders[slot].module=*(VkShaderModule *)(uintptr_t)input[3];
            pass_shaders[slot].fingerprint=fingerprint;
            fprintf(stderr,"[hades-capture] SHADER_FINGERPRINT module=%#llx bytes=%zu fnv64=%#llx; correlation fingerprint, not cryptographic verification\n",
                (unsigned long long)pass_shaders[slot].module,info->codeSize,(unsigned long long)fingerprint);
        }
    } else if (index==HADES_HOST_VULKAN_DESTROY_SHADER) {
        for (unsigned i=0;i<512;i++)
            if (pass_shaders[i].module==(VkShaderModule)(uintptr_t)input[1])
                memset(&pass_shaders[i],0,sizeof pass_shaders[i]);
    } else if (index==HADES_HOST_VULKAN_CREATE_GRAPHICS_PIPELINES && result[0]==VK_SUCCESS) {
        const VkGraphicsPipelineCreateInfo *infos=(const void *)(uintptr_t)input[3];
        const VkPipeline *pipelines=(const void *)(uintptr_t)input[5];
        static unsigned logged;
        for (unsigned i=0;i<input[2];i++) {
            if (logged>=2048) {
                if (logged==2048) {
                    fprintf(stderr,"[hades-capture] PIPELINE_METADATA_LIMIT limit=2048; further pipeline metadata not observed\n");
                    logged++;
                }
                break;
            }
            logged++;
            const VkGraphicsPipelineCreateInfo *info=&infos[i];
            fprintf(stderr,"[hades-capture] GRAPHICS_PIPELINE pipeline=%#llx pass=%#llx subpass=%u layout=%#llx stages=%u\n",
                (unsigned long long)pipelines[i],(unsigned long long)info->renderPass,info->subpass,
                (unsigned long long)info->layout,info->stageCount);
            for (unsigned s=0;s<info->stageCount && s<8;s++) {
                unsigned shader;
                for (shader=0;shader<512 && pass_shaders[shader].module!=info->pStages[s].module;shader++);
                if (shader==512) {
                    status=vk_error("Pass capture pipeline stage has no live shader fingerprint");
                    break;
                }
                fprintf(stderr,"[hades-capture] PIPELINE_STAGE pipeline=%#llx stage=%#x module=%#llx fnv64=%#llx\n",
                    (unsigned long long)pipelines[i],info->pStages[s].stage,
                    (unsigned long long)info->pStages[s].module,(unsigned long long)pass_shaders[shader].fingerprint);
            }
            const VkPipelineColorBlendStateCreateInfo *blend=info->pRasterizationState
                && !info->pRasterizationState->rasterizerDiscardEnable ? info->pColorBlendState : NULL;
            if (blend)
                for (unsigned a=0;a<blend->attachmentCount && a<16;a++) {
                    const VkPipelineColorBlendAttachmentState *b=&blend->pAttachments[a];
                    fprintf(stderr,"[hades-capture] PIPELINE_BLEND pipeline=%#llx attachment=%u enabled=%u rgb=%u/%u/%u alpha=%u/%u/%u mask=%#x\n",
                        (unsigned long long)pipelines[i],a,b->blendEnable,b->srcColorBlendFactor,
                        b->dstColorBlendFactor,b->colorBlendOp,b->srcAlphaBlendFactor,
                        b->dstAlphaBlendFactor,b->alphaBlendOp,b->colorWriteMask);
                }
        }
    } else if (index==HADES_HOST_VULKAN_BIND_PIPELINE && pass_frame_selected()) {
        static unsigned logged;
        if (logged++<1024)
            fprintf(stderr,"[hades-capture] PASS_PIPELINE_BIND frame=%lu command=%#llx pipeline=%#llx\n",
                pass_capture_frame,(unsigned long long)input[0],(unsigned long long)input[2]);
        else if (logged==1025)
            fprintf(stderr,"[hades-capture] PASS_PIPELINE_BIND_LIMIT limit=1024; further bindings not observed\n");
    } else if (index==HADES_HOST_VULKAN_CREATE_IMAGE_VIEW && result[0]==VK_SUCCESS) {
        unsigned slot;
        for (slot=0;slot<2048 && pass_views[slot].view;slot++);
        if (slot==2048) status=vk_error("Pass capture image-view registry exhausted");
        else {
            pass_views[slot].view=*(VkImageView *)(uintptr_t)input[3];
            pass_views[slot].info=*(const VkImageViewCreateInfo *)(uintptr_t)input[1];
        }
    } else if (index==HADES_HOST_VULKAN_CREATE_RENDER_PASS && result[0]==VK_SUCCESS) {
        const VkRenderPassCreateInfo *info=(const void *)(uintptr_t)input[1];
        unsigned slot;
        for (slot=0;slot<512 && pass_passes[slot].pass;slot++);
        if (slot==512 || info->attachmentCount>16) status=vk_error("Pass capture render-pass registry/count exhausted");
        else {
            pass_passes[slot].pass=*(VkRenderPass *)(uintptr_t)input[3];
            pass_passes[slot].count=info->attachmentCount;
            if (info->attachmentCount)
                memcpy(pass_passes[slot].attachments,info->pAttachments,info->attachmentCount*sizeof(VkAttachmentDescription));
        }
    } else if (index==HADES_HOST_VULKAN_CREATE_FRAMEBUFFER && result[0]==VK_SUCCESS) {
        const VkFramebufferCreateInfo *info=(const void *)(uintptr_t)input[1];
        unsigned slot;
        for (slot=0;slot<512 && pass_frames[slot].framebuffer;slot++);
        if (slot==512 || info->attachmentCount>16 || (info->flags&VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT))
            status=vk_error("Pass capture framebuffer registry/count or imageless framebuffer unsupported");
        else {
            pass_frames[slot].framebuffer=*(VkFramebuffer *)(uintptr_t)input[3];
            pass_frames[slot].device=(VkDevice)(uintptr_t)input[0];pass_frames[slot].pass=info->renderPass;
            pass_frames[slot].count=info->attachmentCount;pass_frames[slot].width=info->width;
            pass_frames[slot].height=info->height;pass_frames[slot].layers=info->layers;
            if (info->attachmentCount)
                memcpy(pass_frames[slot].views,info->pAttachments,info->attachmentCount*sizeof(VkImageView));
        }
    } else if (index==HADES_HOST_VULKAN_DESTROY_IMAGE_VIEW) {
        for (unsigned i=0;i<2048;i++)
            if (pass_views[i].view==(VkImageView)(uintptr_t)input[1]) memset(&pass_views[i],0,sizeof pass_views[i]);
    } else if (index==HADES_HOST_VULKAN_DESTROY_RENDER_PASS) {
        for (unsigned i=0;i<512;i++)
            if (pass_passes[i].pass==(VkRenderPass)(uintptr_t)input[1]) memset(&pass_passes[i],0,sizeof pass_passes[i]);
    } else if (index==HADES_HOST_VULKAN_DESTROY_FRAMEBUFFER) {
        for (unsigned i=0;i<512;i++)
            if (pass_frames[i].framebuffer==(VkFramebuffer)(uintptr_t)input[1]) memset(&pass_frames[i],0,sizeof pass_frames[i]);
    } else if (index==HADES_HOST_VULKAN_BEGIN_RENDER_PASS && pass_frame_selected()) {
        unsigned slot;
        for (slot=0;slot<128 && pass_commands[slot].command
            && pass_commands[slot].command!=(VkCommandBuffer)(uintptr_t)input[0];slot++);
        if (slot==128) status=vk_error("Pass capture command registry exhausted");
        else {
            const VkRenderPassBeginInfo *info=(const void *)(uintptr_t)input[1];
            pass_commands[slot].command=(VkCommandBuffer)(uintptr_t)input[0];
            pass_commands[slot].framebuffer=info->framebuffer;pass_commands[slot].pass=info->renderPass;
            pass_commands[slot].draws=0;
            fprintf(stderr,"[hades-capture] PASS_BEGIN frame=%lu command=%#llx framebuffer=%#llx pass=%#llx\n",
                pass_capture_frame,(unsigned long long)input[0],(unsigned long long)info->framebuffer,
                (unsigned long long)info->renderPass);
        }
    } else if (index==HADES_HOST_VULKAN_DRAW_INDEXED || index==HADES_HOST_VULKAN_END_RENDER_PASS) {
        for (unsigned i=0;i<128;i++)
            if (pass_commands[i].command==(VkCommandBuffer)(uintptr_t)input[0] && pass_commands[i].framebuffer) {
                if (index==HADES_HOST_VULKAN_DRAW_INDEXED) {
                    pass_commands[i].draws++;
                    static unsigned logged;
                    if (logged++<2048)
                        fprintf(stderr,"[hades-capture] PASS_DRAW frame=%lu command=%#llx framebuffer=%#llx pass=%#llx indices=%u instances=%u first-index=%u vertex-offset=%d first-instance=%u\n",
                            pass_capture_frame,(unsigned long long)input[0],
                            (unsigned long long)pass_commands[i].framebuffer,(unsigned long long)pass_commands[i].pass,
                            (unsigned)input[1],(unsigned)input[2],(unsigned)input[3],(int32_t)input[4],(unsigned)input[5]);
                    else if (logged==2049)
                        fprintf(stderr,"[hades-capture] PASS_DRAW_LIMIT limit=2048; further draw metadata not observed\n");
                }
                else {
                    status=record_pass_pixels(pass_commands[i].command,pass_commands[i].framebuffer,
                        pass_commands[i].pass,pass_commands[i].draws);
                    memset(&pass_commands[i],0,sizeof pass_commands[i]);
                }
                break;
            }
    } else if (index==HADES_HOST_VULKAN_QUEUE_SUBMIT && result[0]==VK_SUCCESS) {
        status=flush_pass_pixels((VkQueue)(uintptr_t)input[0],(uint32_t)input[1],(const void *)(uintptr_t)input[2]);
    }
    pthread_mutex_unlock(&capture_lock);
    return status;
}

static int write_sampled_texture(id<MTLTexture> source,const char *path) {
    if (source.textureType==MTLTextureType2DArray) {
        source=[source newTextureViewWithPixelFormat:source.pixelFormat textureType:MTLTextureType2D
            levels:NSMakeRange(0,1) slices:NSMakeRange(0,1)];
    }
    if (!source || source.textureType!=MTLTextureType2D || source.sampleCount!=1
        || !(source.usage&MTLTextureUsageShaderRead) || !source.width || !source.height) {
        fprintf(stderr,"[hades-capture] Texture cannot be read as a normalized 2D sample\n");
        return -1;
    }
    static id<MTLComputePipelineState> pipeline;
    NSError *error=nil;
    if (!pipeline) {
        id<MTLLibrary> library=[source.device newLibraryWithSource:
        @"#include <metal_stdlib>\nusing namespace metal;\n"
         "kernel void snapshot(texture2d<float,access::read> src[[texture(0)]],"
         "texture2d<float,access::write> dst[[texture(1)]],uint2 p[[thread_position_in_grid]]){"
         "if(p.x>=dst.get_width()||p.y>=dst.get_height())return;"
         "uint2 q=min(uint2((float2(p)+0.5f)*float2(src.get_width(),src.get_height())/"
         "float2(dst.get_width(),dst.get_height())),uint2(src.get_width()-1,src.get_height()-1));"
         "dst.write(src.read(q),p);}\n" options:nil error:&error];
        id<MTLFunction> function=[library newFunctionWithName:@"snapshot"];
        pipeline=function ? [source.device newComputePipelineStateWithFunction:function error:&error] : nil;
        if (!pipeline) {
            fprintf(stderr,"[hades-capture] Cannot compile host sample-readback kernel: %s\n",
                error.localizedDescription.UTF8String);
            return -1;
        }
    }
    if (pipeline.device!=source.device) {
        fprintf(stderr,"[hades-capture] Snapshot pipeline/device ownership mismatch\n");
        return -1;
    }
    MTLTextureDescriptor *descriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
        width:MIN(source.width,64u) height:MIN(source.height,64u) mipmapped:NO];
    descriptor.storageMode=MTLStorageModePrivate;
    descriptor.usage=MTLTextureUsageShaderWrite|MTLTextureUsageShaderRead;
    id<MTLTexture> output=[source.device newTextureWithDescriptor:descriptor];
    id<MTLCommandQueue> queue=[source.device newCommandQueue];
    id<MTLCommandBuffer> command=[queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
    if (!output || !command || !encoder) {
        fprintf(stderr,"[hades-capture] Cannot allocate actual sample-readback GPU resources\n");
        return -1;
    }
    [encoder setComputePipelineState:pipeline];
    [encoder setTexture:source atIndex:0];
    [encoder setTexture:output atIndex:1];
    [encoder dispatchThreadgroups:MTLSizeMake((output.width+7)/8,(output.height+7)/8,1)
        threadsPerThreadgroup:MTLSizeMake(8,8,1)];
    [encoder endEncoding];
    [command commit];[command waitUntilCompleted];
    if (command.status!=MTLCommandBufferStatusCompleted) {
        fprintf(stderr,"[hades-capture] GPU normalized texture sample failed: %s\n",
            command.error.localizedDescription.UTF8String);
        return -1;
    }
    return write_texture(output,path,YES);
}

static int capture_image_samples(unsigned long frame) {
    char path[PATH_MAX];
    int length=snprintf(path,sizeof path,"%s/textures-%lu.tsv",capture_directory,frame);
    if (length<0 || length>=(int)sizeof path)
        return -1;
    int descriptor=open(path,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
    FILE *manifest=descriptor<0 ? NULL : fdopen(descriptor,"wb");
    if (!manifest) {
        if (descriptor>=0 && close(descriptor))
            fprintf(stderr,"[hades-capture] Cannot close failed texture manifest descriptor\n");
        fprintf(stderr,"[hades-capture] Cannot exclusively create texture snapshot manifest\n");
        return -1;
    }
    int failed=fprintf(manifest,"serial\timage\tformat\twidth\theight\tusage\tstatus\n")<0;
    pthread_mutex_lock(&capture_lock);
    for (unsigned i=0;i<CAPTURE_IMAGE_LIMIT && !failed;i++) {
        if (!capture_images[i].image)
            continue;
        VkFormat format=capture_images[i].format;
        int supported=format==VK_FORMAT_R8_UNORM || format==VK_FORMAT_R8G8_UNORM
            || format==VK_FORMAT_R8G8B8A8_UNORM || format==VK_FORMAT_R8G8B8A8_SRGB
            || format==VK_FORMAT_B8G8R8A8_UNORM || format==VK_FORMAT_B8G8R8A8_SRGB
            || format==VK_FORMAT_BC7_UNORM_BLOCK || format==VK_FORMAT_BC7_SRGB_BLOCK;
        const char *status="unsupported-format-or-usage";
        if (supported && (capture_images[i].usage&VK_IMAGE_USAGE_SAMPLED_BIT)) {
            id<MTLTexture> texture=nil;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            vkGetMTLTextureMVK(capture_images[i].image,&texture);
#pragma clang diagnostic pop
            if (!texture || (texture.textureType!=MTLTextureType2D && texture.textureType!=MTLTextureType2DArray)
                || texture.sampleCount!=1) {
                status="unavailable-or-unsupported-native-texture";
            } else {
                int length=snprintf(path,sizeof path,"%s/image-%lu-frame-%lu.ppm",capture_directory,
                    capture_images[i].serial,frame);
                if (length<0 || length>=(int)sizeof path || write_sampled_texture(texture,path)) {
                    status="failed";
                    failed=1;
                } else {
                    status="written-base-mip-first-layer-nearest-RGB-alpha-thumbnail";
                }
            }
        }
        if (fprintf(manifest,"%lu\t%#llx\t%u\t%u\t%u\t%#x\t%s\n",capture_images[i].serial,
            (unsigned long long)capture_images[i].image,format,capture_images[i].extent.width,
            capture_images[i].extent.height,capture_images[i].usage,status)<0)
            failed=1;
    }
    pthread_mutex_unlock(&capture_lock);
    if (fclose(manifest))
        failed=1;
    fprintf(stderr,"[hades-capture] TEXTURE_SNAPSHOT frame=%lu status=%s registry-overflows=%u; frame-end base storage, not per-pass output or view sampling proof\n",
        frame,failed ? "failed" : "written",capture_image_overflows);
    return failed ? -1 : 0;
}

static int capture_present(VkQueue queue,const VkPresentInfoKHR *present) {
    unsigned long number=atomic_fetch_add_explicit(&capture_presents,1,memory_order_relaxed)+1;
    if (!capture_step || number%capture_step || number/capture_step>4)
        return 0;
    if (!present || present->swapchainCount!=1 || !present->pSwapchains || !present->pImageIndices)
        return -1;
    VkSwapchainKHR swapchain=present->pSwapchains[0];
    VkDevice device=VK_NULL_HANDLE;
    pthread_mutex_lock(&capture_lock);
    for (unsigned i=0;i<32;i++)
        if (capture_swapchains[i].swapchain==swapchain)
            device=capture_swapchains[i].device;
    pthread_mutex_unlock(&capture_lock);
    if (!device || vkQueueWaitIdle(queue)!=VK_SUCCESS) {
        fprintf(stderr,"[hades-capture] Missing owned swapchain or failed render-queue completion\n");
        return -1;
    }
    uint32_t count=16;
    VkImage images[16];
    if (vkGetSwapchainImagesKHR(device,swapchain,&count,images)!=VK_SUCCESS || present->pImageIndices[0]>=count)
        return -1;
    id<MTLTexture> texture=nil;
    /* The pinned static library exposes this documented compatibility API
       directly, not through Vulkan's loader/GPA table. Capture-only use. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    vkGetMTLTextureMVK(images[present->pImageIndices[0]],&texture);
#pragma clang diagnostic pop
    char path[PATH_MAX];
    int size=snprintf(path,sizeof path,"%s/frame-%lu.ppm",capture_directory,number);
    if (size<0 || size>=(int)sizeof path)
        return -1;
    if (write_texture(texture,path,NO))
        return -1;
    return capture_image_samples(number);
}
static const char *const names[] = {
#define HADES_BINDING(operation, name) name,
#include "host_bindings.def"
#include "vulkan_bindings.def"
#include "vulkan_optional.def"
#undef HADES_BINDING
};

static int vk_error(const char *message) {
    fprintf(stderr, "[hades-vulkan] %s\n", message);
    return -1;
}

typedef struct { VkAllocationCallbacks guest, native; } allocator_context;
static allocator_context allocators[64];
static unsigned allocator_count;
static pthread_mutex_t allocator_lock=PTHREAD_MUTEX_INITIALIZER;
static uint64_t (*callback_runner)(uintptr_t,const uint64_t[6]);
static struct {
    VkInstance instance;
    VkDebugUtilsMessengerEXT messenger;
    const VkAllocationCallbacks *allocator;
    PFN_vkDebugUtilsMessengerCallbackEXT function;
    void *user;
    unsigned active;
    int used, closing;
} debug_contexts[32];
static pthread_mutex_t debug_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t debug_idle=PTHREAD_COND_INITIALIZER;

static VkBool32 VKAPI_CALL debug_notify(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type,const VkDebugUtilsMessengerCallbackDataEXT *data,void *user) {
    unsigned slot=(unsigned)(uintptr_t)user;
    pthread_mutex_lock(&debug_lock);
    if (slot>=32 || !debug_contexts[slot].used || !callback_runner) {
        pthread_mutex_unlock(&debug_lock);
        vk_error("Debug notification has no live guest callback owner");
        abort();
    }
    PFN_vkDebugUtilsMessengerCallbackEXT function=debug_contexts[slot].function;
    uint64_t args[6]={severity,type,(uintptr_t)data,(uintptr_t)debug_contexts[slot].user,0,0};
    debug_contexts[slot].active++;
    pthread_mutex_unlock(&debug_lock);
    VkBool32 result=(VkBool32)callback_runner((uintptr_t)function,args);
    pthread_mutex_lock(&debug_lock);
    debug_contexts[slot].active--;
    pthread_cond_broadcast(&debug_idle);
    pthread_mutex_unlock(&debug_lock);
    return result;
}

int hades_vulkan_set_callback_runner(uint64_t (*runner)(uintptr_t,const uint64_t[6])) {
    if (!runner || callback_runner || bootstrap)
        return vk_error("Callback runner must be configured once before Vulkan preparation");
    callback_runner=runner;
    return 0;
}

static void *VKAPI_CALL callback_allocate(void *user,size_t size,size_t alignment,VkSystemAllocationScope scope) {
    allocator_context *context=user;
    uint64_t args[6]={(uintptr_t)context->guest.pUserData,size,alignment,scope,0,0};
    return (void *)(uintptr_t)callback_runner((uintptr_t)context->guest.pfnAllocation,args);
}

static void *VKAPI_CALL callback_reallocate(void *user,void *original,size_t size,size_t alignment,VkSystemAllocationScope scope) {
    allocator_context *context=user;
    uint64_t args[6]={(uintptr_t)context->guest.pUserData,(uintptr_t)original,size,alignment,scope,0};
    return (void *)(uintptr_t)callback_runner((uintptr_t)context->guest.pfnReallocation,args);
}

static void VKAPI_CALL callback_free(void *user,void *memory) {
    allocator_context *context=user;
    uint64_t args[6]={(uintptr_t)context->guest.pUserData,(uintptr_t)memory,0,0,0,0};
    callback_runner((uintptr_t)context->guest.pfnFree,args);
}

static void VKAPI_CALL callback_internal_allocate(void *user,size_t size,VkInternalAllocationType type,VkSystemAllocationScope scope) {
    allocator_context *context=user;
    uint64_t args[6]={(uintptr_t)context->guest.pUserData,size,type,scope,0,0};
    callback_runner((uintptr_t)context->guest.pfnInternalAllocation,args);
}

static void VKAPI_CALL callback_internal_free(void *user,size_t size,VkInternalAllocationType type,VkSystemAllocationScope scope) {
    allocator_context *context=user;
    uint64_t args[6]={(uintptr_t)context->guest.pUserData,size,type,scope,0,0};
    callback_runner((uintptr_t)context->guest.pfnInternalFree,args);
}

static const VkAllocationCallbacks *adapt_allocator(const VkAllocationCallbacks *source) {
    if (!source)
        return NULL;
    hades_memory_info info;
    uintptr_t address=(uintptr_t)source;
    if (!callback_runner || hades_memory_info_query(address,&info) || !(info.permissions&1)
        || address<info.base || sizeof *source>info.size-(address-info.base)) {
        vk_error("Allocator callback structure is unreadable or no guest runner configured");
        return NULL;
    }
    VkAllocationCallbacks guest=*source;
    if (!guest.pfnAllocation || !guest.pfnReallocation || !guest.pfnFree
        || !!guest.pfnInternalAllocation!=!!guest.pfnInternalFree) {
        vk_error("Incomplete guest Vulkan allocation callbacks");
        return NULL;
    }
    pthread_mutex_lock(&allocator_lock);
    for (unsigned i=0;i<allocator_count;i++) {
        const VkAllocationCallbacks *old=&allocators[i].guest;
        if (old->pUserData==guest.pUserData && old->pfnAllocation==guest.pfnAllocation
            && old->pfnReallocation==guest.pfnReallocation && old->pfnFree==guest.pfnFree
            && old->pfnInternalAllocation==guest.pfnInternalAllocation && old->pfnInternalFree==guest.pfnInternalFree) {
            pthread_mutex_unlock(&allocator_lock);
            return &allocators[i].native;
        }
    }
    if (allocator_count==64) {
        pthread_mutex_unlock(&allocator_lock);
        vk_error("Process-lifetime immutable allocator context capacity exhausted");
        return NULL;
    }
    allocator_context *context=&allocators[allocator_count++];
    context->guest=guest;
    context->native=(VkAllocationCallbacks){.pUserData=context,.pfnAllocation=callback_allocate,
        .pfnReallocation=callback_reallocate,.pfnFree=callback_free,
        .pfnInternalAllocation=guest.pfnInternalAllocation ? callback_internal_allocate : NULL,
        .pfnInternalFree=guest.pfnInternalFree ? callback_internal_free : NULL};
    pthread_mutex_unlock(&allocator_lock);
    fprintf(stderr,"[hades-vulkan] GUEST_ALLOCATOR_CAPTURED: typed callbacks, immutable process-lifetime context\n");
    return &context->native;
}

int hades_vulkan_configure_surface(void *layer) {
    if (bootstrap || surface_layer || !layer || ![(__bridge id)layer isKindOfClass:CAMetalLayer.class])
        return vk_error("Surface configuration requires a real, previously unconfigured CAMetalLayer");
    surface_layer=(__bridge CAMetalLayer *)layer;
    return 0;
}

int hades_vulkan_prepare(void *layer) {
    if (bootstrap)
        return vk_error("Vulkan/Metal backend is one-shot; duplicate preparation refused");
    const char *completion=getenv("HADES_VULKAN_QUEUE_COMPLETION_PROBE");
    if (completion && strcmp(completion,"0") && strcmp(completion,"1"))
        return vk_error("Invalid queue completion probe setting; expected 0 or 1");
    queue_completion_probe=completion && !strcmp(completion,"1");
    if (queue_completion_probe)
        fprintf(stderr,"[hades-vulkan] QUEUE_COMPLETION_PROBE enabled; serial GPU completion changes timing, not a production fix\n");
    const char *loads=getenv("HADES_VULKAN_ATTACHMENT_LOAD_PROBE");
    if (loads && strcmp(loads,"0") && strcmp(loads,"1"))
        return vk_error("Invalid attachment load probe setting; expected 0 or 1");
    attachment_load_probe=loads && !strcmp(loads,"1");
    if (attachment_load_probe)
        fprintf(stderr,"[hades-vulkan] ATTACHMENT_LOAD_PROBE enabled; preserves DONT_CARE color contents only for color-to-color layouts, not a confirmed fix\n");
    const char *pass_frame=getenv("HADES_VULKAN_PASS_CAPTURE_FRAME");
    if (pass_frame) {
        char *end=NULL;
        unsigned long frame=strtoul(pass_frame,&end,10);
        if (!pass_frame[0] || *end || frame>100000 || (frame && !capture_step))
            return vk_error("Invalid pass capture frame or missing private capture configuration");
        pass_capture_frame=frame;
        if (frame) {
            fprintf(stderr,"[hades-capture] PASS_CAPTURE_ENABLED frame=%lu limit=%u budget=384MiB; inserts copies/barriers and waits, not a fix\n",
                frame,PASS_READ_LIMIT);
            fprintf(stderr,"[hades-capture] PASS_FLOW_ENABLED frames=%lu/%lu calls=16384; second window inserts no pass copies, CPU logging still perturbs timing\n",
                frame,frame*2);
        }
    }
    metal_device = MTLCreateSystemDefaultDevice();
    if (!metal_device)
        return vk_error("No actual Metal device");
    if (layer && hades_vulkan_configure_surface(layer))
        return -1;
    int headless=!surface_layer;
    if (headless)
        surface_layer = [CAMetalLayer layer];
    surface_layer.device = metal_device;
    surface_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    surface_layer.framebufferOnly = NO;
    if (headless)
        surface_layer.drawableSize = CGSizeMake(1280,720);
    if (hades_ipc_set_display_size((uint32_t)surface_layer.drawableSize.width,(uint32_t)surface_layer.drawableSize.height))
        return vk_error("Cannot publish actual host drawable extent");
    const char *extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME,VK_EXT_METAL_SURFACE_EXTENSION_NAME,VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
    VkApplicationInfo application = {.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName="Hades local Vulkan adapter",.apiVersion=VK_API_VERSION_1_1};
    VkInstanceCreateInfo info = {.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo=&application,.enabledExtensionCount=3,.ppEnabledExtensionNames=extensions};
    VkResult result = vkCreateInstance(&info,NULL,&bootstrap);
    if (result != VK_SUCCESS)
        return vk_error("Real MoltenVK bootstrap instance creation failed");
    uint32_t count=0;
    if (vkEnumeratePhysicalDevices(bootstrap,&count,NULL) != VK_SUCCESS || !count)
        return vk_error("No actual Vulkan/Metal physical device");
    fprintf(stderr,"[hades-vulkan] REAL_METAL_BACKEND_READY device=%s; unattached surface is not onscreen evidence\n",
            metal_device.name.UTF8String);
    fprintf(stderr,"[hades-vulkan] METAL_TEXTURE_CAPABILITY BC=%u\n",metal_device.supportsBCTextureCompression);
    return 0;
}

static unsigned entry_index(const char *name) {
    for (unsigned i=HADES_HOST_VULKAN_PROPERTIES;i<HADES_HOST_COUNT;i++)
        if (!strcmp(names[i],name))
            return i;
    size_t length=strlen(name);
    if (length>3 && !strcmp(name+length-3,"KHR"))
        for (unsigned i=HADES_HOST_VULKAN_PROPERTIES;i<HADES_HOST_COUNT;i++)
            if (strlen(names[i])==length-3 && !strncmp(names[i],name,length-3))
                return i;
    return HADES_HOST_COUNT;
}

static int create_instance(uint64_t *r) {
    const VkAllocationCallbacks *allocator=adapt_allocator((const void *)(uintptr_t)r[1]);
    if (r[1] && !allocator)
        return -1;
    const VkInstanceCreateInfo *source=(const void *)(uintptr_t)r[0];
    if (!source || source->enabledExtensionCount>128 || source->enabledLayerCount || source->pNext)
        return vk_error("Invalid instance info or unadapted instance layers/pNext callbacks");
    const char *extensions[132];
    uint32_t count=0;
    for (uint32_t i=0;i<source->enabledExtensionCount;i++) {
        const char *name=source->ppEnabledExtensionNames[i];
        if (!strcmp(name,VK_NN_VI_SURFACE_EXTENSION_NAME))
            continue;
        if (!strcmp(name,VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) || !strcmp(name,VK_EXT_METAL_SURFACE_EXTENSION_NAME))
            continue;
        extensions[count++]=name;
    }
    extensions[count++]=VK_EXT_METAL_SURFACE_EXTENSION_NAME;
    VkInstanceCreateInfo info=*source;
    info.flags&=~VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    info.enabledExtensionCount=count;
    info.ppEnabledExtensionNames=extensions;
    r[0]=(uint32_t)vkCreateInstance(&info,allocator,(VkInstance *)(uintptr_t)r[2]);
    return 0;
}

static int create_device(uint64_t *r) {
    const VkAllocationCallbacks *allocator=adapt_allocator((const void *)(uintptr_t)r[2]);
    if (r[2] && !allocator)
        return -1;
    const VkDeviceCreateInfo *source=(const void *)(uintptr_t)r[1];
    if (!source || source->enabledExtensionCount>128 || source->enabledLayerCount)
        return vk_error("Invalid device info or unimplemented guest layers");
    const char *extensions[129];
    uint32_t count=0;
    for (uint32_t i=0;i<source->enabledExtensionCount;i++)
        if (strcmp(source->ppEnabledExtensionNames[i],"VK_KHR_portability_subset"))
            extensions[count++]=source->ppEnabledExtensionNames[i];
    extensions[count++]="VK_KHR_portability_subset";
    VkDeviceCreateInfo info=*source;
    info.enabledExtensionCount=count;
    info.ppEnabledExtensionNames=extensions;
    r[0]=(uint32_t)vkCreateDevice((VkPhysicalDevice)(uintptr_t)r[0],&info,allocator,(VkDevice *)(uintptr_t)r[3]);
    return 0;
}

enum { descriptor_types=11 };
static struct {
    VkDescriptorSetLayout handle;
    VkDevice device;
    const VkAllocationCallbacks *allocator;
    uint32_t counts[descriptor_types];
} descriptor_layouts[512];
typedef struct descriptor_batch {
    struct descriptor_batch *next;
    VkDescriptorPool backing;
    uint32_t count;
    VkDescriptorSet sets[];
} descriptor_batch;
typedef struct {
    VkDescriptorPool handle;
    VkDevice device;
    const VkAllocationCallbacks *allocator;
    VkDescriptorPoolCreateFlags flags;
    uint32_t capacity, allocated;
    descriptor_batch *batches;
    int used, busy;
} descriptor_pool;
static descriptor_pool descriptor_pools[256];
static pthread_mutex_t descriptor_lock=PTHREAD_MUTEX_INITIALIZER;

static descriptor_pool *acquire_descriptor_pool(VkDevice device,VkDescriptorPool handle) {
    pthread_mutex_lock(&descriptor_lock);
    descriptor_pool *pool=NULL;
    for (unsigned i=0;i<256;i++)
        if (descriptor_pools[i].used && descriptor_pools[i].handle==handle
            && descriptor_pools[i].device==device && !descriptor_pools[i].busy) {
            pool=&descriptor_pools[i];
            pool->busy=1;
            break;
        }
    pthread_mutex_unlock(&descriptor_lock);
    if (!pool)
        vk_error("Descriptor pool has no live owner or lacks required external synchronization");
    return pool;
}

static void release_descriptor_pool(descriptor_pool *pool,int destroyed) {
    pthread_mutex_lock(&descriptor_lock);
    if (destroyed)
        memset(pool,0,sizeof *pool);
    else
        pool->busy=0;
    pthread_mutex_unlock(&descriptor_lock);
}

static void discard_descriptor_batches(descriptor_pool *pool) {
    descriptor_batch *batch=pool->batches;
    while (batch) {
        descriptor_batch *next=batch->next;
        if (batch->backing!=pool->handle)
            vkDestroyDescriptorPool(pool->device,batch->backing,pool->allocator);
        free(batch);
        batch=next;
    }
    pool->batches=NULL;
    pool->allocated=0;
}

static int descriptor_layout_call(unsigned index,uint64_t *r) {
    VkDevice device=(VkDevice)(uintptr_t)r[0];
    const VkAllocationCallbacks *allocator=adapt_allocator((const void *)(uintptr_t)r[2]);
    if (r[2] && !allocator)
        return -1;
    if (index==HADES_HOST_VULKAN_DESTROY_DESCRIPTOR_LAYOUT) {
        VkDescriptorSetLayout handle=(VkDescriptorSetLayout)(uintptr_t)r[1];
        if (!handle)
            return 0;
        pthread_mutex_lock(&descriptor_lock);
        unsigned slot;
        for (slot=0;slot<512 && descriptor_layouts[slot].handle!=handle;slot++);
        if (slot==512 || descriptor_layouts[slot].device!=device || descriptor_layouts[slot].allocator!=allocator) {
            pthread_mutex_unlock(&descriptor_lock);
            return vk_error("Descriptor layout destruction lacks matching owner/allocator");
        }
        memset(&descriptor_layouts[slot],0,sizeof descriptor_layouts[slot]);
        pthread_mutex_unlock(&descriptor_lock);
        vkDestroyDescriptorSetLayout(device,handle,allocator);
        return 0;
    }
    const VkDescriptorSetLayoutCreateInfo *info=(const void *)(uintptr_t)r[1];
    if (!info || !r[3] || info->sType!=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO
        || info->pNext || info->flags || (info->bindingCount && !info->pBindings))
        return vk_error("Descriptor layout requires adapted fixed-count bindings without extensions/flags");
    uint32_t counts[descriptor_types]={0};
    for (unsigned i=0;i<info->bindingCount;i++) {
        VkDescriptorType type=info->pBindings[i].descriptorType;
        uint32_t count=info->pBindings[i].descriptorCount;
        if ((unsigned)type>=descriptor_types || count>UINT32_MAX-counts[type])
            return vk_error("Descriptor layout type/count is unsupported or overflows");
        counts[type]+=count;
    }
    VkDescriptorSetLayout handle=VK_NULL_HANDLE;
    VkResult result=vkCreateDescriptorSetLayout(device,info,allocator,&handle);
    if (result==VK_SUCCESS) {
        pthread_mutex_lock(&descriptor_lock);
        unsigned slot;
        for (slot=0;slot<512 && descriptor_layouts[slot].handle;slot++);
        if (slot<512) {
            descriptor_layouts[slot].handle=handle;
            descriptor_layouts[slot].device=device;
            descriptor_layouts[slot].allocator=allocator;
            memcpy(descriptor_layouts[slot].counts,counts,sizeof counts);
        }
        pthread_mutex_unlock(&descriptor_lock);
        if (slot==512) {
            vkDestroyDescriptorSetLayout(device,handle,allocator);
            return vk_error("Descriptor layout ownership registry exhausted");
        }
    }
    *(VkDescriptorSetLayout *)(uintptr_t)r[3]=handle;
    r[0]=(uint32_t)result;
    return 0;
}

static int descriptor_pool_create(uint64_t *r) {
    const VkDescriptorPoolCreateInfo *info=(const void *)(uintptr_t)r[1];
    const VkAllocationCallbacks *allocator=adapt_allocator((const void *)(uintptr_t)r[2]);
    if (r[2] && !allocator)
        return -1;
    if (!info || !r[3] || info->sType!=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO
        || info->pNext || !info->maxSets || (info->flags&~VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT)
        || (info->poolSizeCount && !info->pPoolSizes))
        return vk_error("Descriptor pool requires adapted fixed-count resources/flags");
    VkDevice device=(VkDevice)(uintptr_t)r[0];
    pthread_mutex_lock(&descriptor_lock);
    unsigned slot;
    for (slot=0;slot<256 && descriptor_pools[slot].used;slot++);
    if (slot<256)
        descriptor_pools[slot].used=1;
    pthread_mutex_unlock(&descriptor_lock);
    if (slot==256)
        return vk_error("Descriptor pool ownership registry exhausted");
    descriptor_pool *pool=&descriptor_pools[slot];
    VkDescriptorPool handle=VK_NULL_HANDLE;
    VkResult result=vkCreateDescriptorPool(device,info,allocator,&handle);
    if (result==VK_SUCCESS) {
        pthread_mutex_lock(&descriptor_lock);
        pool->handle=handle;
        pool->device=device;
        pool->allocator=allocator;
        pool->flags=info->flags;
        pool->capacity=info->maxSets;
        pthread_mutex_unlock(&descriptor_lock);
        fprintf(stderr,"[hades-vulkan] DESCRIPTOR_POOL_CREATED max-sets=%u types=%u\n",
            info->maxSets,info->poolSizeCount);
    } else {
        release_descriptor_pool(pool,1);
    }
    *(VkDescriptorPool *)(uintptr_t)r[3]=handle;
    r[0]=(uint32_t)result;
    return 0;
}

static int descriptor_sets_allocate(descriptor_pool *pool,uint64_t *r) {
    const VkDescriptorSetAllocateInfo *info=(const void *)(uintptr_t)r[1];
    VkDescriptorSet *output=(void *)(uintptr_t)r[2];
    if (!info || !output || info->sType!=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
        || info->pNext || !info->descriptorSetCount || !info->pSetLayouts)
        return vk_error("Invalid or unadapted descriptor set allocation");
    uint32_t count=info->descriptorSetCount;
    if (count>pool->capacity-pool->allocated) {
        memset(output,0,(size_t)count*sizeof *output);
        r[0]=(uint32_t)VK_ERROR_OUT_OF_POOL_MEMORY;
        fprintf(stderr,"[hades-vulkan] Descriptor set allocation exceeds actual guest maxSets budget\n");
        return 0;
    }
    uint32_t demand[descriptor_types]={0};
    pthread_mutex_lock(&descriptor_lock);
    for (uint32_t i=0;i<count;i++) {
        unsigned slot;
        for (slot=0;slot<512 && descriptor_layouts[slot].handle!=info->pSetLayouts[i];slot++);
        if (slot==512 || descriptor_layouts[slot].device!=pool->device) {
            pthread_mutex_unlock(&descriptor_lock);
            return vk_error("Descriptor allocation has an unowned layout");
        }
        for (unsigned type=0;type<descriptor_types;type++) {
            if (descriptor_layouts[slot].counts[type]>UINT32_MAX-demand[type]) {
                pthread_mutex_unlock(&descriptor_lock);
                return vk_error("Actual descriptor batch resource demand overflows");
            }
            demand[type]+=descriptor_layouts[slot].counts[type];
        }
    }
    pthread_mutex_unlock(&descriptor_lock);
    descriptor_batch *batch=malloc(sizeof *batch+(size_t)count*sizeof *output);
    if (!batch) {
        memset(output,0,(size_t)count*sizeof *output);
        r[0]=(uint32_t)VK_ERROR_OUT_OF_HOST_MEMORY;
        fprintf(stderr,"[hades-vulkan] Cannot allocate descriptor batch ownership records\n");
        return 0;
    }
    batch->backing=pool->handle;
    VkResult result=vkAllocateDescriptorSets(pool->device,info,output);
    /* Hades requests 6144 sets from pools whose per-type estimates can be 1024.
       Provision real driver resources from recorded layout demand, not fake sets
       or a guessed multiplier; preserve maxSets and the complete pool lifetime. */
    if (result==VK_ERROR_OUT_OF_POOL_MEMORY || result==VK_ERROR_FRAGMENTED_POOL) {
        VkDescriptorPoolSize sizes[descriptor_types];
        uint32_t types=0;
        for (unsigned type=0;type<descriptor_types;type++)
            if (demand[type])
                sizes[types++]=(VkDescriptorPoolSize){(VkDescriptorType)type,demand[type]};
        VkDescriptorPoolCreateInfo backing_info={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .flags=pool->flags,.maxSets=count,.poolSizeCount=types,.pPoolSizes=sizes};
        result=vkCreateDescriptorPool(pool->device,&backing_info,pool->allocator,&batch->backing);
        if (result==VK_SUCCESS) {
            VkDescriptorSetAllocateInfo allocation=*info;
            allocation.descriptorPool=batch->backing;
            result=vkAllocateDescriptorSets(pool->device,&allocation,output);
            if (result==VK_SUCCESS) {
                fprintf(stderr,"[hades-vulkan] DESCRIPTOR_BATCH_PROVISIONED real-sets=%u types=%u within-max-sets=%u\n",
                    count,types,pool->capacity);
                for (unsigned i=0;i<types;i++)
                    fprintf(stderr,"[hades-vulkan] DESCRIPTOR_BATCH_RESOURCE type=%u count=%u\n",
                        sizes[i].type,sizes[i].descriptorCount);
            } else {
                vkDestroyDescriptorPool(pool->device,batch->backing,pool->allocator);
            }
        }
    }
    if (result==VK_SUCCESS) {
        batch->count=count;
        memcpy(batch->sets,output,(size_t)count*sizeof *output);
        batch->next=pool->batches;
        pool->batches=batch;
        pool->allocated+=count;
    } else {
        free(batch);
        memset(output,0,(size_t)count*sizeof *output);
        fprintf(stderr,"[hades-vulkan] DESCRIPTOR_ALLOCATION_FAILED sets=%u result=%d; outputs cleared, no fabricated success\n",
            count,result);
    }
    r[0]=(uint32_t)result;
    return 0;
}

static int descriptor_pool_call(unsigned index,uint64_t *r) {
    if (index==HADES_HOST_VULKAN_CREATE_DESCRIPTOR_POOL)
        return descriptor_pool_create(r);
    VkDevice device=(VkDevice)(uintptr_t)r[0];
    const VkDescriptorSetAllocateInfo *info=index==HADES_HOST_VULKAN_ALLOCATE_DESCRIPTOR_SETS
        ? (const void *)(uintptr_t)r[1] : NULL;
    VkDescriptorPool handle=info ? info->descriptorPool : (VkDescriptorPool)(uintptr_t)r[1];
    if (index==HADES_HOST_VULKAN_DESTROY_DESCRIPTOR_POOL && !handle)
        return 0;
    descriptor_pool *pool=acquire_descriptor_pool(device,handle);
    if (!pool)
        return -1;
    int status=0,destroyed=0;
    if (index==HADES_HOST_VULKAN_ALLOCATE_DESCRIPTOR_SETS) {
        status=descriptor_sets_allocate(pool,r);
    } else if (index==HADES_HOST_VULKAN_DESTROY_DESCRIPTOR_POOL) {
        const VkAllocationCallbacks *allocator=adapt_allocator((const void *)(uintptr_t)r[2]);
        if ((r[2] && !allocator) || allocator!=pool->allocator) {
            status=vk_error("Descriptor pool destruction lacks matching allocator");
        } else {
            discard_descriptor_batches(pool);
            vkDestroyDescriptorPool(device,handle,allocator);
            destroyed=1;
        }
    } else if (index==HADES_HOST_VULKAN_RESET_DESCRIPTOR_POOL) {
        if (r[2]) {
            status=vk_error("Unsupported descriptor pool reset flags");
        } else {
            VkResult result=vkResetDescriptorPool(device,handle,0);
            if (result==VK_SUCCESS)
                discard_descriptor_batches(pool);
            else
                fprintf(stderr,"[hades-vulkan] Descriptor pool reset failed result=%d\n",result);
            r[0]=(uint32_t)result;
        }
    } else {
        uint32_t count=(uint32_t)r[2];
        const VkDescriptorSet *sets=(const void *)(uintptr_t)r[3];
        if (!(pool->flags&VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT) || !sets || !count
            || count>pool->allocated) {
            status=vk_error("Invalid descriptor set free count, flags or pointer");
        } else {
            for (unsigned i=0;i<count && !status;i++) {
                unsigned matches=0;
                for (unsigned j=0;j<i;j++)
                    if (sets[j]==sets[i])
                        status=vk_error("Descriptor set free includes a duplicate handle");
                for (descriptor_batch *batch=pool->batches;batch;batch=batch->next)
                    for (unsigned j=0;j<batch->count;j++)
                        if (sets[i] && batch->sets[j]==sets[i])
                            matches++;
                if (matches!=1)
                    status=vk_error("Descriptor set free has no matching live pool owner");
            }
            VkResult result=VK_SUCCESS;
            for (unsigned i=0;i<count && !status && result==VK_SUCCESS;i++)
                for (descriptor_batch *batch=pool->batches;batch;batch=batch->next)
                    for (unsigned j=0;j<batch->count;j++)
                        if (batch->sets[j]==sets[i]) {
                            result=vkFreeDescriptorSets(device,batch->backing,1,&sets[i]);
                            if (result==VK_SUCCESS) {
                                batch->sets[j]=VK_NULL_HANDLE;
                                pool->allocated--;
                            }
                        }
            if (result!=VK_SUCCESS)
                fprintf(stderr,"[hades-vulkan] Descriptor set free failed result=%d\n",result);
            descriptor_batch **link=&pool->batches;
            while (*link) {
                descriptor_batch *batch=*link;
                unsigned remaining=0;
                for (unsigned i=0;i<batch->count;i++)
                    remaining+=batch->sets[i]!=VK_NULL_HANDLE;
                if (remaining) {
                    link=&batch->next;
                } else {
                    *link=batch->next;
                    if (batch->backing!=pool->handle)
                        vkDestroyDescriptorPool(device,batch->backing,pool->allocator);
                    free(batch);
                }
            }
            r[0]=(uint32_t)result;
        }
    }
    release_descriptor_pool(pool,destroyed);
    return status;
}

static int invoke(unsigned index, PFN_vkVoidFunction function, uint64_t *r) {
    if (index==HADES_HOST_VULKAN_UPDATE_DESCRIPTOR_TEMPLATE && !r[1])
        return vk_error("Descriptor template update received a null set; prior allocation failure must be resolved");
    switch (index) {
#include "vulkan_dispatch.inc"
        default: return vk_error("Vulkan call lacks a typed AOT forwarding adapter");
    }
}

#ifdef HADES_VULKAN_TESTING
int hades_vulkan_test_texture_readback(const char *path) {
    id<MTLDevice> device=MTLCreateSystemDefaultDevice();
    MTLTextureDescriptor *descriptor=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
        width:16 height:16 mipmapped:NO];
    descriptor.storageMode=MTLStorageModeShared;
    id<MTLTexture> texture=[device newTextureWithDescriptor:descriptor];
    uint8_t pixels[16*16*4];
    for (unsigned i=0;i<16*16;i++) {
        pixels[i*4]=11;pixels[i*4+1]=22;pixels[i*4+2]=33;pixels[i*4+3]=255;
    }
    [texture replaceRegion:MTLRegionMake2D(0,0,16,16) mipmapLevel:0 withBytes:pixels bytesPerRow:64];
    return write_texture(texture,path,NO);
}

int hades_vulkan_test_capture_images(unsigned long frame) {
    return capture_image_samples(frame);
}

int hades_vulkan_test_pass_window(unsigned long completed,unsigned long entered) {
    return pass_counts_selected(completed,entered);
}

unsigned long hades_vulkan_test_flow_window(unsigned long completed,unsigned long entered) {
    return flow_counts_selected(completed,entered);
}

unsigned hades_vulkan_test_static_load(uintptr_t pc,uint32_t page,uint32_t load,uintptr_t *address) {
    return flow_static_load(pc,page,load,address);
}

unsigned hades_vulkan_test_static_samples(uintptr_t caller) {
    return pass_capture_frame ? trace_flow_static_loads(0,caller) : 0;
}

int hades_vulkan_test_forge_batch(const uint64_t *r) {
    return pass_capture_frame ? trace_flow_forge_batch(0,r) : 0;
}

unsigned hades_vulkan_test_preserve_color_loads(VkAttachmentDescription *attachments,unsigned count) {
    return preserve_color_loads(attachments,count);
}

int hades_vulkan_test_surface_geometry(void) {
    CAMetalLayer *layer=[CAMetalLayer layer];
    layer.drawableSize=CGSizeMake(640,360);
    if (hades_vulkan_configure_surface((__bridge void *)layer) || hades_vulkan_prepare(NULL))
        return -1;
    return layer.drawableSize.width==640 && layer.drawableSize.height==360 ? 0
        : vk_error("Backend overwrote explicitly configured frontend drawable geometry");
}

int hades_vulkan_test_invoke(unsigned index, PFN_vkVoidFunction function, uint64_t *r) {
    return invoke(index,function,r);
}
#endif

int hades_vulkan_call(unsigned index, const char *name, uint64_t *r) {
    @autoreleasepool {
        trace_pass_flow(index,name,r);
        if (!bootstrap)
            return vk_error("Vulkan call before explicitly configured real backend");
        if (index==HADES_HOST_GRAPHICS_INITIALIZE) {
            if (graphics_ready || !r[1] || !hades_memory_writable(r[0],r[1]))
                return vk_error("Invalid/duplicate graphics initialization pool");
            graphics_ready=1;
            fprintf(stderr,"[hades-vulkan] NVIDIA public bootstrap adapted to actual Vulkan/Metal device; vendor pool remains guest-owned\n");
            r[0]=0;
            return 0;
        }
        if (!graphics_ready)
            return vk_error("Graphics interface before initialization");
        if (index==HADES_HOST_VI_INITIALIZE) {
            vi_ready=1;
            r[0]=0;
            return 0;
        }
        if (index==HADES_HOST_VI_DISPLAY || index==HADES_HOST_VI_LAYER || index==HADES_HOST_VI_WINDOW) {
            if (!vi_ready || !hades_memory_writable(r[0],8)
                || (index==HADES_HOST_VI_LAYER && r[1]!=(uintptr_t)&display_token)
                || (index==HADES_HOST_VI_WINDOW && r[1]!=(uintptr_t)&layer_token))
                return vk_error("Invalid owned display/layer/window request");
            uintptr_t value=index==HADES_HOST_VI_DISPLAY ? (uintptr_t)&display_token
                : index==HADES_HOST_VI_LAYER ? (uintptr_t)&layer_token : (uintptr_t)&window_token;
            memcpy((void *)(uintptr_t)r[0],&value,8);
            r[0]=0;
            return 0;
        }
        if (index==HADES_HOST_VULKAN_VI_SURFACE) {
            const VkViSurfaceCreateInfoNN *source=(const void *)(uintptr_t)r[1];
            if (!source || source->sType!=VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN
                || source->pNext || source->flags || source->window!=&window_token)
                return vk_error("Invalid native window/VI surface extension request");
            const VkAllocationCallbacks *allocator=adapt_allocator((const void *)(uintptr_t)r[2]);
            if (r[2] && !allocator)
                return -1;
            VkMetalSurfaceCreateInfoEXT info={.sType=VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT,
                                              .pLayer=surface_layer};
            r[0]=(uint32_t)vkCreateMetalSurfaceEXT((VkInstance)(uintptr_t)r[0],&info,allocator,(VkSurfaceKHR *)(uintptr_t)r[3]);
            return 0;
        }
        if (index==HADES_HOST_VULKAN_INSTANCE_PROC || index==HADES_HOST_VULKAN_DEVICE_PROC) {
            const char *requested=(const void *)(uintptr_t)r[1];
            unsigned target=entry_index(requested);
            PFN_vkVoidFunction native=index==HADES_HOST_VULKAN_INSTANCE_PROC
                ? vkGetInstanceProcAddr((VkInstance)(uintptr_t)r[0],requested)
                : vkGetDeviceProcAddr((VkDevice)(uintptr_t)r[0],requested);
            if (target==HADES_HOST_VULKAN_VI_SURFACE && r[0])
                native=(PFN_vkVoidFunction)hades_host_entrypoint(target);
            r[0]=native && target<HADES_HOST_COUNT ? (uintptr_t)hades_host_entrypoint(target) : 0;
            if (native && target==HADES_HOST_COUNT)
                fprintf(stderr,"[hades-vulkan] unadapted optional function not exposed: %s\n",requested);
            return 0;
        }
        if (index==HADES_HOST_VULKAN_CREATE_INSTANCE)
            return create_instance(r);
        if (index==HADES_HOST_VULKAN_CREATE_DEVICE) {
            pass_physical=(VkPhysicalDevice)(uintptr_t)r[0];
            return create_device(r);
        }
        if (index==HADES_HOST_VULKAN_CREATE_DESCRIPTOR_LAYOUT || index==HADES_HOST_VULKAN_DESTROY_DESCRIPTOR_LAYOUT)
            return descriptor_layout_call(index,r);
        if (index==HADES_HOST_VULKAN_CREATE_DESCRIPTOR_POOL || index==HADES_HOST_VULKAN_DESTROY_DESCRIPTOR_POOL
            || index==HADES_HOST_VULKAN_ALLOCATE_DESCRIPTOR_SETS || index==HADES_HOST_VULKAN_FREE_DESCRIPTOR_SETS
            || index==HADES_HOST_VULKAN_RESET_DESCRIPTOR_POOL)
            return descriptor_pool_call(index,r);
        if (index==HADES_HOST_VULKAN_DEBUG_MESSENGER_CREATE) {
            const VkDebugUtilsMessengerCreateInfoEXT *source=(const void *)(uintptr_t)r[1];
            if (!callback_runner || !source || source->sType!=VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT
                || source->pNext || !source->pfnUserCallback || !hades_memory_writable(r[3],sizeof(VkDebugUtilsMessengerEXT)))
                return vk_error("Invalid or unadapted guest debug messenger configuration");
            const VkAllocationCallbacks *allocator=adapt_allocator((const void *)(uintptr_t)r[2]);
            if (r[2] && !allocator)
                return -1;
            PFN_vkCreateDebugUtilsMessengerEXT create=(PFN_vkCreateDebugUtilsMessengerEXT)
                vkGetInstanceProcAddr((VkInstance)(uintptr_t)r[0],name);
            if (!create)
                return vk_error("Driver does not expose requested debug messenger extension");
            pthread_mutex_lock(&debug_lock);
            unsigned slot;
            for (slot=0;slot<32 && debug_contexts[slot].used;slot++);
            if (slot==32) {
                pthread_mutex_unlock(&debug_lock);
                return vk_error("Guest debug messenger capacity exhausted");
            }
            debug_contexts[slot].used=1;
            debug_contexts[slot].instance=(VkInstance)(uintptr_t)r[0];
            debug_contexts[slot].function=source->pfnUserCallback;
            debug_contexts[slot].user=source->pUserData;
            debug_contexts[slot].allocator=allocator;
            pthread_mutex_unlock(&debug_lock);
            VkDebugUtilsMessengerCreateInfoEXT info=*source;
            info.pfnUserCallback=debug_notify;
            info.pUserData=(void *)(uintptr_t)slot;
            VkResult result=create((VkInstance)(uintptr_t)r[0],&info,allocator,(VkDebugUtilsMessengerEXT *)(uintptr_t)r[3]);
            pthread_mutex_lock(&debug_lock);
            if (result==VK_SUCCESS)
                debug_contexts[slot].messenger=*(VkDebugUtilsMessengerEXT *)(uintptr_t)r[3];
            else
                memset(&debug_contexts[slot],0,sizeof debug_contexts[slot]);
            pthread_mutex_unlock(&debug_lock);
            r[0]=(uint32_t)result;
            return 0;
        }
        if (index==HADES_HOST_VULKAN_DEBUG_MESSENGER_DESTROY) {
            const VkAllocationCallbacks *allocator=adapt_allocator((const void *)(uintptr_t)r[2]);
            if (r[2] && !allocator)
                return -1;
            pthread_mutex_lock(&debug_lock);
            unsigned slot;
            for (slot=0;slot<32 && (!debug_contexts[slot].used
                || debug_contexts[slot].messenger!=(VkDebugUtilsMessengerEXT)(uintptr_t)r[1]);slot++);
            if (slot==32 || debug_contexts[slot].instance!=(VkInstance)(uintptr_t)r[0]
                || debug_contexts[slot].allocator!=allocator || debug_contexts[slot].closing) {
                pthread_mutex_unlock(&debug_lock);
                return vk_error("Debug messenger destruction lacks matching owner/allocator");
            }
            debug_contexts[slot].closing=1;
            pthread_mutex_unlock(&debug_lock);
            PFN_vkDestroyDebugUtilsMessengerEXT destroy=(PFN_vkDestroyDebugUtilsMessengerEXT)
                vkGetInstanceProcAddr((VkInstance)(uintptr_t)r[0],name);
            if (!destroy)
                return vk_error("Driver lost debug messenger destruction function");
            destroy((VkInstance)(uintptr_t)r[0],(VkDebugUtilsMessengerEXT)(uintptr_t)r[1],allocator);
            pthread_mutex_lock(&debug_lock);
            while (debug_contexts[slot].active)
                if (pthread_cond_wait(&debug_idle,&debug_lock)) {
                    pthread_mutex_unlock(&debug_lock);
                    return vk_error("Cannot drain live debug notifications during destroy");
                }
            memset(&debug_contexts[slot],0,sizeof debug_contexts[slot]);
            pthread_mutex_unlock(&debug_lock);
            return 0;
        }
        if (index==HADES_HOST_VULKAN_INSTANCE_EXTENSIONS) {
            if (r[0])
                return vk_error("Guest extension layer enumeration is not supported");
            uint32_t count=0;
            VkResult result=vkEnumerateInstanceExtensionProperties(NULL,&count,NULL);
            if (result!=VK_SUCCESS)
                return vk_error("Real instance extension enumeration failed");
            VkExtensionProperties *properties=calloc(count+1,sizeof *properties);
            if (!properties)
                return vk_error("Cannot allocate real extension enumeration");
            result=vkEnumerateInstanceExtensionProperties(NULL,&count,properties);
            if (result!=VK_SUCCESS) {
                free(properties);
                return vk_error("Real extension inventory changed/failed");
            }
            strlcpy(properties[count].extensionName,VK_NN_VI_SURFACE_EXTENSION_NAME,sizeof properties[count].extensionName);
            properties[count++].specVersion=1;
            uint32_t *written=(void *)(uintptr_t)r[1];
            if (!r[2]) {
                *written=count;
                r[0]=VK_SUCCESS;
            } else {
                uint32_t available=*written;
                uint32_t copied=available<count ? available : count;
                memcpy((void *)(uintptr_t)r[2],properties,copied*sizeof *properties);
                *written=copied;
                r[0]=available<count ? VK_INCOMPLETE : VK_SUCCESS;
            }
            free(properties);
            return 0;
        }
        PFN_vkVoidFunction function=vkGetInstanceProcAddr(bootstrap,name);
        if (!function)
            return vk_error("Requested imported Vulkan API unavailable in real backend");
        uint64_t input[8];
        memcpy(input,r,sizeof input);
        if (pass_capture_frame && index==HADES_HOST_VULKAN_BEGIN_COMMAND_BUFFER) {
            pthread_mutex_lock(&capture_lock);
            int pending=0;
            for (unsigned i=0;i<pass_read_count;i++)
                pending|=pass_reads[i].buffer && pass_reads[i].command==(VkCommandBuffer)(uintptr_t)input[0];
            pthread_mutex_unlock(&capture_lock);
            if (pending)
                return vk_error("Pass capture command reset/re-recorded before its recorded copies were submitted");
        }
        if (capture_step && index==HADES_HOST_VULKAN_CREATE_SWAPCHAIN) {
            VkSwapchainCreateInfoKHR info=*(const VkSwapchainCreateInfoKHR *)(uintptr_t)input[1];
            info.imageUsage|=VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            const VkAllocationCallbacks *allocator=adapt_allocator((const void *)(uintptr_t)input[2]);
            if (input[2] && !allocator)
                return -1;
            r[0]=(uint32_t)((PFN_vkCreateSwapchainKHR)function)((VkDevice)(uintptr_t)input[0],&info,
                allocator,(VkSwapchainKHR *)(uintptr_t)input[3]);
            if (r[0]==VK_SUCCESS) {
                pthread_mutex_lock(&capture_lock);
                unsigned slot;
                for (slot=0;slot<32 && capture_swapchains[slot].swapchain;slot++);
                if (slot<32) {
                    capture_swapchains[slot].device=(VkDevice)(uintptr_t)input[0];
                    capture_swapchains[slot].swapchain=*(VkSwapchainKHR *)(uintptr_t)input[3];
                }
                pthread_mutex_unlock(&capture_lock);
                if (slot==32) {
                    vkDestroySwapchainKHR((VkDevice)(uintptr_t)input[0],*(VkSwapchainKHR *)(uintptr_t)input[3],allocator);
                    fprintf(stderr,"[hades-capture] Owned swapchain registry exhausted\n");
                    return -1;
                }
            }
            return 0;
        }
        if (capture_step && index==HADES_HOST_VULKAN_QUEUE_PRESENT
            && capture_present((VkQueue)(uintptr_t)input[0],(const VkPresentInfoKHR *)(uintptr_t)input[1]))
            return -1;
        int memory_call=index==HADES_HOST_VULKAN_ALLOCATE_MEMORY || index==HADES_HOST_VULKAN_MAP_MEMORY
            || index==HADES_HOST_VULKAN_UNMAP_MEMORY || index==HADES_HOST_VULKAN_FREE_MEMORY;
        if (memory_call)
            pthread_mutex_lock(&resource_lock);
        if (index==HADES_HOST_VULKAN_MAP_MEMORY || index==HADES_HOST_VULKAN_UNMAP_MEMORY
            || (index==HADES_HOST_VULKAN_FREE_MEMORY && input[1])) {
            unsigned slot;
            for (slot=0;slot<1024 && allocations[slot].memory!=(VkDeviceMemory)(uintptr_t)input[1];slot++);
            if (slot==1024 || !input[1] || allocations[slot].device!=(VkDevice)(uintptr_t)input[0]
                || (index==HADES_HOST_VULKAN_FREE_MEMORY
                    && (adapt_allocator((const void *)(uintptr_t)input[2])!=allocations[slot].allocator
                        || (input[2] && !allocations[slot].allocator)))
                || (index==HADES_HOST_VULKAN_UNMAP_MEMORY && !allocations[slot].mapped)
                || (index==HADES_HOST_VULKAN_MAP_MEMORY && (allocations[slot].mapped
                    || input[2]>=allocations[slot].size
                    || (input[3]!=VK_WHOLE_SIZE && (!input[3] || input[3]>allocations[slot].size-input[2]))))) {
                pthread_mutex_unlock(&resource_lock);
                return vk_error("Invalid mapped-allocation ownership, device, range or lifecycle");
            }
            if (index!=HADES_HOST_VULKAN_MAP_MEMORY && allocations[slot].mapped) {
                if (hades_memory_unregister_external(0x4000+slot)) {
                    pthread_mutex_unlock(&resource_lock);
                    return -1;
                }
                allocations[slot].mapped=0;
            }
        }
        int destroying_capture_image=capture_step && index==HADES_HOST_VULKAN_DESTROY_IMAGE;
        if (destroying_capture_image)
            pthread_mutex_lock(&capture_lock);
        VkRenderPassCreateInfo load_info;
        VkAttachmentDescription *load_attachments=NULL;
        VkImageCreateInfo pass_image;
        int adapted_pass_image=0;
        if (pass_capture_frame && index==HADES_HOST_VULKAN_CREATE_IMAGE) {
            const VkImageCreateInfo *source=(const void *)(uintptr_t)input[1];
            if (source->samples==VK_SAMPLE_COUNT_1_BIT
                && (source->format==VK_FORMAT_R8G8B8A8_UNORM || source->format==VK_FORMAT_B8G8R8A8_UNORM)) {
                pass_image=*source;pass_image.usage|=VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
                r[1]=(uintptr_t)&pass_image;adapted_pass_image=1;
            }
        }
        if (attachment_load_probe && index==HADES_HOST_VULKAN_CREATE_RENDER_PASS) {
            const VkRenderPassCreateInfo *source=(const void *)(uintptr_t)input[1];
            if (!source || source->sType!=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO
                || (source->attachmentCount && !source->pAttachments)
                || source->attachmentCount>256) {
                if (memory_call)
                    pthread_mutex_unlock(&resource_lock);
                return vk_error("Invalid render-pass attachment load probe input");
            }
            load_info=*source;
            if (source->attachmentCount) {
                load_attachments=malloc(source->attachmentCount*sizeof *load_attachments);
                if (!load_attachments)
                    return vk_error("Cannot allocate attachment load probe copy");
                memcpy(load_attachments,source->pAttachments,source->attachmentCount*sizeof *load_attachments);
                unsigned changed=preserve_color_loads(load_attachments,source->attachmentCount);
                load_info.pAttachments=load_attachments;
                static _Atomic unsigned logged;
                if (changed && atomic_fetch_add_explicit(&logged,1,memory_order_relaxed)<32)
                    fprintf(stderr,"[hades-vulkan] ATTACHMENT_LOAD_PROBE_CHANGED count=%u; original guest declaration remains unchanged\n",changed);
            }
            r[1]=(uintptr_t)&load_info;
        }
        int status=invoke(index,function,r);
        if (adapted_pass_image || (attachment_load_probe && index==HADES_HOST_VULKAN_CREATE_RENDER_PASS))
            r[1]=input[1];
        free(load_attachments);
        if (destroying_capture_image) {
            if (!status)
                for (unsigned i=0;i<CAPTURE_IMAGE_LIMIT;i++)
                    if (capture_images[i].image==(VkImage)(uintptr_t)input[1])
                        memset(&capture_images[i],0,sizeof capture_images[i]);
            pthread_mutex_unlock(&capture_lock);
        }
        if (status) {
            if (memory_call)
                pthread_mutex_unlock(&resource_lock);
            return status;
        }
        if (!memory_call && track_pass_call(index,input,r))
            return -1;
        if (queue_completion_probe && index==HADES_HOST_VULKAN_QUEUE_SUBMIT && r[0]==VK_SUCCESS) {
            VkResult completion=vkQueueWaitIdle((VkQueue)(uintptr_t)input[0]);
            if (completion!=VK_SUCCESS) {
                fprintf(stderr,"[hades-vulkan] QUEUE_COMPLETION_PROBE wait failed result=%d\n",completion);
                r[0]=(uint32_t)completion;
            } else {
                static _Atomic unsigned long completions;
                unsigned long number=atomic_fetch_add_explicit(&completions,1,memory_order_relaxed)+1;
                if (number<=3 || !(number%600))
                    fprintf(stderr,"[hades-vulkan] QUEUE_COMPLETION_PROBE_WAIT_OK submission=%lu\n",number);
            }
        }
        if (index==HADES_HOST_VULKAN_FEATURES2) {
            const VkPhysicalDeviceFeatures2 *features=(const void *)(uintptr_t)input[1];
            fprintf(stderr,"[hades-vulkan] TEXTURE_COMPRESSION_FEATURES BC=%u ETC2=%u ASTC-LDR=%u\n",
                features->features.textureCompressionBC,features->features.textureCompressionETC2,
                features->features.textureCompressionASTC_LDR);
        }
        if (index==HADES_HOST_VULKAN_FORMAT_PROPERTIES) {
            static _Atomic unsigned queries;
            static _Atomic unsigned char formats[VK_FORMAT_ASTC_12x12_SRGB_BLOCK+1];
            int first=input[1]<=VK_FORMAT_ASTC_12x12_SRGB_BLOCK
                ? !atomic_exchange_explicit(&formats[input[1]],1,memory_order_relaxed)
                : atomic_fetch_add_explicit(&queries,1,memory_order_relaxed)<16;
            if (first) {
                const VkFormatProperties *properties=(const void *)(uintptr_t)input[2];
                fprintf(stderr,"[hades-vulkan] TEXTURE_FORMAT_QUERY format=%u optimal=%#x linear=%#x buffer=%#x\n",
                    (unsigned)input[1],properties->optimalTilingFeatures,properties->linearTilingFeatures,
                    properties->bufferFeatures);
            }
        }
        if (index==HADES_HOST_VULKAN_CREATE_IMAGE && r[0]==VK_SUCCESS) {
            if (capture_step) {
                const VkImageCreateInfo *info=(const void *)(uintptr_t)input[1];
                pthread_mutex_lock(&capture_lock);
                unsigned slot;
                for (slot=0;slot<CAPTURE_IMAGE_LIMIT && capture_images[slot].image;slot++);
                if (slot<CAPTURE_IMAGE_LIMIT) {
                    capture_images[slot].image=*(VkImage *)(uintptr_t)input[3];
                    capture_images[slot].format=info->format;
                    capture_images[slot].extent=info->extent;
                    capture_images[slot].usage=info->usage;
                    capture_images[slot].serial=++capture_image_serial;
                } else {
                    capture_image_overflows++;
                    if (capture_image_overflows<=3)
                        fprintf(stderr,"[hades-capture] Texture registry capped at %u live images; additional images not captured\n",CAPTURE_IMAGE_LIMIT);
                }
                pthread_mutex_unlock(&capture_lock);
            }
            static _Atomic unsigned images;
            if (atomic_fetch_add_explicit(&images,1,memory_order_relaxed)<(capture_step ? CAPTURE_IMAGE_LIMIT : 128)) {
                const VkImageCreateInfo *info=(const void *)(uintptr_t)input[1];
                fprintf(stderr,"[hades-vulkan] TEXTURE_IMAGE_CREATED handle=%#llx format=%u extent=%ux%ux%u mip=%u layers=%u usage=%#x tiling=%u\n",
                    (unsigned long long)*(VkImage *)(uintptr_t)input[3],info->format,
                    info->extent.width,info->extent.height,info->extent.depth,info->mipLevels,
                    info->arrayLayers,info->usage,info->tiling);
            }
        }
        if (index==HADES_HOST_VULKAN_CREATE_IMAGE_VIEW && r[0]==VK_SUCCESS) {
            static _Atomic unsigned views;
            if (atomic_fetch_add_explicit(&views,1,memory_order_relaxed)<(capture_step ? CAPTURE_IMAGE_LIMIT : 128)) {
                const VkImageViewCreateInfo *info=(const void *)(uintptr_t)input[1];
                fprintf(stderr,"[hades-vulkan] TEXTURE_VIEW_CREATED view=%#llx image=%#llx format=%u swizzle=%u/%u/%u/%u mip=%u+%u layers=%u+%u\n",
                    (unsigned long long)*(VkImageView *)(uintptr_t)input[3],
                    (unsigned long long)info->image,info->format,info->components.r,info->components.g,
                    info->components.b,info->components.a,info->subresourceRange.baseMipLevel,
                    info->subresourceRange.levelCount,info->subresourceRange.baseArrayLayer,info->subresourceRange.layerCount);
            }
        }
        if (capture_step && index==HADES_HOST_VULKAN_CREATE_RENDER_PASS && r[0]==VK_SUCCESS) {
            static _Atomic unsigned passes;
            if (atomic_fetch_add_explicit(&passes,1,memory_order_relaxed)<32) {
                const VkRenderPassCreateInfo *info=(const void *)(uintptr_t)input[1];
                fprintf(stderr,"[hades-capture] RENDER_PASS handle=%#llx attachments=%u dependencies=%u\n",
                    (unsigned long long)*(VkRenderPass *)(uintptr_t)input[3],info->attachmentCount,info->dependencyCount);
                for (unsigned i=0;i<info->attachmentCount && i<16;i++) {
                    const VkAttachmentDescription *a=&info->pAttachments[i];
                    fprintf(stderr,"[hades-capture] ATTACHMENT index=%u format=%u load=%u store=%u stencil-load=%u stencil-store=%u layout=%u->%u\n",
                        i,a->format,a->loadOp,a->storeOp,a->stencilLoadOp,a->stencilStoreOp,a->initialLayout,a->finalLayout);
                }
                for (unsigned i=0;i<info->dependencyCount && i<32;i++) {
                    const VkSubpassDependency *d=&info->pDependencies[i];
                    fprintf(stderr,"[hades-capture] PASS_DEPENDENCY subpass=%u->%u stages=%#x->%#x access=%#x->%#x flags=%#x\n",
                        d->srcSubpass,d->dstSubpass,d->srcStageMask,d->dstStageMask,d->srcAccessMask,d->dstAccessMask,d->dependencyFlags);
                }
            }
        }
        if (capture_step && index==HADES_HOST_VULKAN_PIPELINE_BARRIER) {
            static _Atomic unsigned barriers;
            if (atomic_fetch_add_explicit(&barriers,1,memory_order_relaxed)<64) {
                uint32_t count=(uint32_t)r[100];
                const VkImageMemoryBarrier *images=(const void *)(uintptr_t)r[101];
                fprintf(stderr,"[hades-capture] PIPELINE_BARRIER command=%#llx stages=%#x->%#x flags=%#x memories=%u buffers=%u images=%u\n",
                    (unsigned long long)input[0],(unsigned)input[1],(unsigned)input[2],(unsigned)input[3],
                    (unsigned)input[4],(unsigned)input[6],count);
                for (unsigned i=0;i<count && i<8;i++) {
                    const VkImageMemoryBarrier *image=&images[i];
                    fprintf(stderr,"[hades-capture] IMAGE_BARRIER image=%#llx layout=%u->%u access=%#x->%#x mip=%u+%u layers=%u+%u\n",
                        (unsigned long long)image->image,image->oldLayout,image->newLayout,
                        image->srcAccessMask,image->dstAccessMask,image->subresourceRange.baseMipLevel,
                        image->subresourceRange.levelCount,image->subresourceRange.baseArrayLayer,image->subresourceRange.layerCount);
                }
            }
        }
        if (capture_step && index==HADES_HOST_VULKAN_DESTROY_SWAPCHAIN) {
            pthread_mutex_lock(&capture_lock);
            for (unsigned i=0;i<32;i++)
                if (capture_swapchains[i].swapchain==(VkSwapchainKHR)(uintptr_t)input[1])
                    memset(&capture_swapchains[i],0,sizeof capture_swapchains[i]);
            pthread_mutex_unlock(&capture_lock);
        }
        if (index==HADES_HOST_VULKAN_ALLOCATE_MEMORY && r[0]==VK_SUCCESS) {
            unsigned slot;
            for (slot=0;slot<1024 && allocations[slot].memory;slot++);
            if (slot==1024) {
                vkFreeMemory((VkDevice)(uintptr_t)input[0],*(VkDeviceMemory *)(uintptr_t)input[3],
                    adapt_allocator((const void *)(uintptr_t)input[2]));
                pthread_mutex_unlock(&resource_lock);
                return vk_error("Owned Vulkan memory registry exhausted");
            }
            allocations[slot].memory=*(VkDeviceMemory *)(uintptr_t)input[3];
            allocations[slot].device=(VkDevice)(uintptr_t)input[0];
            allocations[slot].size=((const VkMemoryAllocateInfo *)(uintptr_t)input[1])->allocationSize;
            allocations[slot].allocator=adapt_allocator((const void *)(uintptr_t)input[2]);
        } else if (index==HADES_HOST_VULKAN_MAP_MEMORY && r[0]==VK_SUCCESS) {
            unsigned slot;
            for (slot=0;slot<1024 && allocations[slot].memory!=(VkDeviceMemory)(uintptr_t)input[1];slot++);
            if (slot==1024 || allocations[slot].mapped || input[2]>allocations[slot].size) {
                pthread_mutex_unlock(&resource_lock);
                return vk_error("Vulkan mapped memory has no owned allocation record");
            }
            size_t size=input[3]==VK_WHOLE_SIZE ? (size_t)(allocations[slot].size-input[2]) : input[3];
            uintptr_t pointer=*(uintptr_t *)(uintptr_t)input[5];
            if (hades_memory_register_external(pointer,size,0x4000+slot)) {
                vkUnmapMemory((VkDevice)(uintptr_t)input[0],(VkDeviceMemory)(uintptr_t)input[1]);
                pthread_mutex_unlock(&resource_lock);
                return -1;
            }
            allocations[slot].mapped=1;
        } else if (index==HADES_HOST_VULKAN_FREE_MEMORY && input[1]) {
            unsigned slot;
            for (slot=0;slot<1024 && allocations[slot].memory!=(VkDeviceMemory)(uintptr_t)input[1];slot++);
            if (slot==1024) {
                pthread_mutex_unlock(&resource_lock);
                return vk_error("Unknown Vulkan allocation during free/unmap");
            }
            memset(&allocations[slot],0,sizeof allocations[slot]);
        } else if (index==HADES_HOST_VULKAN_QUEUE_PRESENT
                   && ((VkResult)r[0]==VK_SUCCESS || (VkResult)r[0]==VK_SUBOPTIMAL_KHR)) {
            unsigned long frame=atomic_fetch_add_explicit(&presents,1,memory_order_relaxed)+1;
            if (frame<=3 || !(frame%600) || (VkResult)r[0]==VK_SUBOPTIMAL_KHR)
                fprintf(stderr,"[hades-vulkan] VULKAN_PRESENT_SUBMITTED %lu result=%d; not proof of visible correct gameplay\n",
                        frame,(int)(VkResult)r[0]);
            if (frame==1 || !(frame%600))
                hades_log_memory("present",frame,metal_device!=nil,metal_device.currentAllocatedSize);
        }
        if (memory_call)
            pthread_mutex_unlock(&resource_lock);
        return 0;
    }
}
