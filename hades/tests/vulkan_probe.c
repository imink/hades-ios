#define VK_USE_PLATFORM_VI_NN
#include <vulkan/vulkan.h>
#include "../runtime/vulkan.h"
#include "../runtime/host.h"
#include "../runtime/memory.h"
#include "../runtime/callout.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>

static _Alignas(16384) unsigned char pool[16384];
static _Alignas(16384) uint32_t observation_code[4096];
static _Alignas(16384) uint64_t observation_data[2048];
static VkImageMemoryBarrier tail_barrier;
static struct { atomic_ulong allocate,release,reallocate,debug; } allocator_counts;
typedef struct { void *base; size_t size; } block_header;
static void *VKAPI_CALL test_allocate(void *user,size_t size,size_t alignment,VkSystemAllocationScope scope) {
    assert(user==&allocator_counts && scope<=VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
    assert(alignment && !(alignment&(alignment-1)));
    if (alignment<sizeof(void *)) alignment=sizeof(void *);
    void *base=NULL;
    assert(!posix_memalign(&base,alignment,size+alignment+sizeof(block_header)));
    uintptr_t pointer=((uintptr_t)base+sizeof(block_header)+alignment-1)&~(uintptr_t)(alignment-1);
    block_header header={base,size};
    memcpy((void *)(pointer-sizeof header),&header,sizeof header);
    atomic_fetch_add(&allocator_counts.allocate,1);
    return (void *)pointer;
}
static void VKAPI_CALL test_free(void *user,void *pointer) {
    assert(user==&allocator_counts);
    if (!pointer) return;
    block_header header;
    memcpy(&header,(char *)pointer-sizeof header,sizeof header);
    free(header.base);
    atomic_fetch_add(&allocator_counts.release,1);
}
static void *VKAPI_CALL test_reallocate(void *user,void *original,size_t size,size_t alignment,VkSystemAllocationScope scope) {
    atomic_fetch_add(&allocator_counts.reallocate,1);
    if (!size) { test_free(user,original);return NULL; }
    void *pointer=test_allocate(user,size,alignment,scope);
    if (original) {
        block_header header;
        memcpy(&header,(char *)original-sizeof header,sizeof header);
        memcpy(pointer,original,size<header.size ? size : header.size);
        test_free(user,original);
    }
    return pointer;
}
static VkBool32 VKAPI_CALL test_debug(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type,const VkDebugUtilsMessengerCallbackDataEXT *data,void *user) {
    assert(user==&allocator_counts && severity && type && data && data->pMessage);
    atomic_fetch_add(&allocator_counts.debug,1);
    return VK_FALSE;
}
static VkResult VKAPI_CALL check_allocator(VkDevice device,const VkBufferCreateInfo *info,
    const VkAllocationCallbacks *allocator,VkBuffer *buffer) {
    assert(!device && !info && allocator && !buffer);
    void *p=allocator->pfnAllocation(allocator->pUserData,37,64,VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    assert(p && !((uintptr_t)p%64));
    memset(p,0x72,37);
    p=allocator->pfnReallocation(allocator->pUserData,p,91,32,VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
    assert(p && !((uintptr_t)p%32));
    for (unsigned i=0;i<37;i++) assert(((unsigned char *)p)[i]==0x72);
    allocator->pfnFree(allocator->pUserData,p);
    return VK_SUCCESS;
}
static uint64_t test_runner(uintptr_t function,const uint64_t args[6]) {
    assert(function==(uintptr_t)test_allocate || function==(uintptr_t)test_reallocate
        || function==(uintptr_t)test_free || function==(uintptr_t)test_debug);
    return hades_callout_run(function,args);
}
void hades_host_dispatch(uint64_t index,uint64_t *registers) {
    (void)index;(void)registers;
    abort();
}
extern int hades_vulkan_test_invoke(unsigned,PFN_vkVoidFunction,uint64_t *);
extern int hades_vulkan_test_surface_geometry(void);
extern int hades_vulkan_test_texture_readback(const char *);
extern int hades_vulkan_test_capture_images(unsigned long);
extern int hades_vulkan_test_pass_window(unsigned long,unsigned long);
extern unsigned long hades_vulkan_test_flow_window(unsigned long,unsigned long);
extern unsigned hades_vulkan_test_static_load(uintptr_t,uint32_t,uint32_t,uintptr_t *);
extern unsigned hades_vulkan_test_static_samples(uintptr_t);
extern int hades_vulkan_test_forge_batch(const uint64_t *);
extern unsigned hades_vulkan_test_preserve_color_loads(VkAttachmentDescription *,unsigned);
static void VKAPI_CALL check_tail(VkCommandBuffer command,VkPipelineStageFlags source,VkPipelineStageFlags destination,
    VkDependencyFlags dependency,uint32_t memories,const VkMemoryBarrier *memory,
    uint32_t buffers,const VkBufferMemoryBarrier *buffer,uint32_t images,const VkImageMemoryBarrier *image) {
    assert(!command && !source && !destination && !dependency && !memories && !memory && !buffers && !buffer);
    assert(images==3 && image==&tail_barrier);
}
static const char *const names[]={
#define HADES_BINDING(operation,name) name,
#include "../runtime/host_bindings.def"
#include "../runtime/vulkan_bindings.def"
#include "../runtime/vulkan_optional.def"
#undef HADES_BINDING
};
const void *hades_host_entrypoint(unsigned index) { return index<HADES_HOST_COUNT ? pool+8*index : NULL; }
static uint64_t call(unsigned index,const uint64_t *arguments,size_t count) {
    uint64_t frame[102]={0};
    assert(count<=10);
    for (size_t i=0;i<count;i++)
        frame[i<8 ? i : 100+i-8]=arguments[i];
    assert(!hades_vulkan_call(index,names[index],frame));
    return frame[0];
}
#define U(value) ((uintptr_t)(value))
#define C(operation,...) call(HADES_HOST_##operation,(uint64_t[]){__VA_ARGS__},sizeof((uint64_t[]){__VA_ARGS__})/8)

static void static_load_probe(int enabled) {
    uintptr_t address=0;
    for (unsigned size=0;size<4;size++) {
        uint32_t load=0x39400000|(size<<30)|(3<<10)|(8<<5)|9;
        assert(hades_vulkan_test_static_load(0x12345004,0x90000008,load,&address)==(1u<<size));
        assert(address==0x12345000+3*(1u<<size));
        assert(hades_vulkan_test_static_load(0x12345004,0xf0ffffe8,load,&address)==(1u<<size));
        assert(address==0x12344000+3*(1u<<size));
        assert(!hades_vulkan_test_static_load(0x12345004,0x90000007,load,&address));
        assert(!hades_vulkan_test_static_load(0x12345004,0x90000008,load|0x04000000,&address));
    }
    assert(!hades_vulkan_test_static_load(0x12345005,0x90000008,0xb9400100,&address));
    assert(!hades_vulkan_test_static_load(1ull<<39,0x90000008,0xb9400100,&address));
    assert(!hades_vulkan_test_static_load(0,0xf0ffffe8,0xb9400100,&address));
    assert(!hades_vulkan_test_static_load(0x12345004,0x10000008,0xb9400100,&address));
    assert(!hades_vulkan_test_static_load(0x12345004,0x9000001f,0xb94003e0,&address));
    int64_t pages=((int64_t)U(observation_data)-(int64_t)U(observation_code))/4096;
    assert(pages>=-(1<<20) && pages<(1<<20));
    uint32_t immediate=(uint32_t)pages&0x1fffff;
    observation_code[0]=0x90000008|((immediate&3)<<29)|((immediate>>2)<<5);
    observation_code[1]=0xf9400109;
    observation_data[0]=0x123456789abcdef0ull;
    assert(!hades_memory_register(U(observation_code),sizeof observation_code,3,5,100));
    assert(!hades_memory_register(U(observation_data),sizeof observation_data,4,3,101));
    assert(hades_vulkan_test_static_samples(U(observation_code))==(unsigned)enabled);
    for (unsigned i=1;i<9;i++) {
        observation_code[i*2]=observation_code[0];
        observation_code[i*2+1]=observation_code[1];
    }
    assert(hades_vulkan_test_static_samples(U(observation_code))==(enabled ? 8u : 0u));
    memset(observation_code+2,0,16*sizeof(uint32_t));
    assert(!hades_vulkan_test_static_samples(U(pool)));
    assert(!hades_vulkan_test_static_samples(U(observation_code)+sizeof observation_code-4));
    assert(!hades_vulkan_test_static_samples(0));
    uint64_t frame[102]={0};
    frame[30]=U(observation_code)+0x100;
    frame[19]=U(observation_data);
    uint32_t fingerprint[]={0x9b082553,0x90000009,0xf9441529,0xb9495e68};
    memcpy((char *)observation_code+0x70,fingerprint,sizeof fingerprint);
    uint32_t value=1234;
    memcpy((char *)observation_data+0x10,&value,4);
    assert(hades_vulkan_test_forge_batch(frame)==enabled);
    observation_code[0x70/4]^=1;
    assert(!hades_vulkan_test_forge_batch(frame));
    observation_code[0x70/4]^=1;
    frame[19]=U(observation_data)+sizeof observation_data-4;
    assert(!hades_vulkan_test_forge_batch(frame));
    frame[19]=U(observation_data);
    assert(!hades_memory_unregister(101));
    assert(!hades_vulkan_test_static_samples(U(observation_code)));
    assert(!hades_vulkan_test_forge_batch(frame));
    assert(!hades_memory_unregister(100));
    puts("FLOW_STATIC_LOAD_OK: decoded signed pages, scalar widths and mapped readable data only");
}

static void descriptor_budget_probe(VkDevice device,VkBuffer buffer,const VkAllocationCallbacks *allocator) {
    VkDescriptorSetLayoutBinding binding={.binding=0,.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        .descriptorCount=2,.stageFlags=VK_SHADER_STAGE_VERTEX_BIT};
    VkDescriptorSetLayoutCreateInfo layout_info={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount=1,.pBindings=&binding};
    VkDescriptorSetLayout layout;
    assert(C(VULKAN_CREATE_DESCRIPTOR_LAYOUT,U(device),U(&layout_info),U(allocator),U(&layout))==VK_SUCCESS);
    VkDescriptorPoolSize size={VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1};
    VkDescriptorPoolCreateInfo pool_info={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags=VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,.maxSets=3,.poolSizeCount=1,.pPoolSizes=&size};
    VkDescriptorPool pool;
    assert(C(VULKAN_CREATE_DESCRIPTOR_POOL,U(device),U(&pool_info),U(allocator),U(&pool))==VK_SUCCESS);
    VkDescriptorSetLayout layouts[3]={layout,layout,layout};
    VkDescriptorSetAllocateInfo allocation={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool=pool,.descriptorSetCount=2,.pSetLayouts=layouts};
    VkDescriptorSet sets[2];
    assert(C(VULKAN_ALLOCATE_DESCRIPTOR_SETS,U(device),U(&allocation),U(sets))==VK_SUCCESS);
    assert(sets[0] && sets[1] && sets[0]!=sets[1]);
    VkDescriptorUpdateTemplateEntry entry={.dstBinding=0,.descriptorCount=2,
        .descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,.stride=sizeof(VkDescriptorBufferInfo)};
    VkDescriptorUpdateTemplateCreateInfo template_info={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO,
        .descriptorUpdateEntryCount=1,.pDescriptorUpdateEntries=&entry,
        .templateType=VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET,.descriptorSetLayout=layout};
    VkDescriptorUpdateTemplate update;
    assert(C(VULKAN_CREATE_DESCRIPTOR_TEMPLATE,U(device),U(&template_info),U(allocator),U(&update))==VK_SUCCESS);
    VkDescriptorBufferInfo buffers[2]={{buffer,0,64},{buffer,64,64}};
    C(VULKAN_UPDATE_DESCRIPTOR_TEMPLATE,U(device),U(sets[0]),U(update),U(buffers));
    VkDescriptorSet rejected[2]={sets[0],sets[1]};
    assert((int32_t)C(VULKAN_ALLOCATE_DESCRIPTOR_SETS,U(device),U(&allocation),U(rejected))==VK_ERROR_OUT_OF_POOL_MEMORY);
    assert(!rejected[0] && !rejected[1]);
    VkDescriptorSet duplicate[2]={sets[0],sets[0]};
    uint64_t invalid[102]={U(device),U(pool),2,U(duplicate)};
    assert(hades_vulkan_call(HADES_HOST_VULKAN_FREE_DESCRIPTOR_SETS,"vkFreeDescriptorSets",invalid)==-1);
    assert(C(VULKAN_FREE_DESCRIPTOR_SETS,U(device),U(pool),1,U(&sets[0]))==VK_SUCCESS);
    invalid[2]=1;
    assert(hades_vulkan_call(HADES_HOST_VULKAN_FREE_DESCRIPTOR_SETS,"vkFreeDescriptorSets",invalid)==-1);
    VkDescriptorSet extra[2];
    assert(C(VULKAN_ALLOCATE_DESCRIPTOR_SETS,U(device),U(&allocation),U(extra))==VK_SUCCESS);
    C(VULKAN_UPDATE_DESCRIPTOR_TEMPLATE,U(device),U(extra[1]),U(update),U(buffers));
    assert(C(VULKAN_RESET_DESCRIPTOR_POOL,U(device),U(pool),0)==VK_SUCCESS);
    allocation.descriptorSetCount=3;
    VkDescriptorSet reset_sets[3];
    assert(C(VULKAN_ALLOCATE_DESCRIPTOR_SETS,U(device),U(&allocation),U(reset_sets))==VK_SUCCESS);
    C(VULKAN_UPDATE_DESCRIPTOR_TEMPLATE,U(device),U(reset_sets[2]),U(update),U(buffers));
    assert(C(VULKAN_FREE_DESCRIPTOR_SETS,U(device),U(pool),3,U(reset_sets))==VK_SUCCESS);
    C(VULKAN_DESTROY_DESCRIPTOR_POOL,U(device),U(pool),U(allocator));
    C(VULKAN_DESTROY_DESCRIPTOR_TEMPLATE,U(device),U(update),U(allocator));
    C(VULKAN_DESTROY_DESCRIPTOR_LAYOUT,U(device),U(layout),U(allocator));
    puts("VULKAN_DESCRIPTOR_BUDGET_OK: real layout-sized backing sets, updates, maxSets failures, free/reset/destroy");
}

static void texture_sample_probe(VkDevice device,VkQueue queue,unsigned family,
    const VkPhysicalDeviceMemoryProperties *memory,VkBuffer source,void *mapped) {
    VkAttachmentDescription attachment={.format=VK_FORMAT_R8G8B8A8_UNORM,.samples=VK_SAMPLE_COUNT_1_BIT,
        .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,.finalLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkAttachmentReference color={0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount=1,.pColorAttachments=&color};
    VkSubpassDependency dependency={.srcSubpass=0,.dstSubpass=VK_SUBPASS_EXTERNAL,
        .srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,.dstStageMask=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        .srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,.dstAccessMask=VK_ACCESS_SHADER_READ_BIT};
    VkRenderPassCreateInfo render_info={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount=1,.pAttachments=&attachment,.subpassCount=1,.pSubpasses=&subpass,
        .dependencyCount=1,.pDependencies=&dependency};
    VkRenderPass render_pass;
    assert(C(VULKAN_CREATE_RENDER_PASS,U(device),U(&render_info),0,U(&render_pass))==VK_SUCCESS);
    VkAttachmentDescription load_cases[5]={attachment,attachment,attachment,attachment,attachment};
    for (unsigned i=0;i<5;i++) {
        load_cases[i].loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        load_cases[i].initialLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        load_cases[i].finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    load_cases[1].loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;
    load_cases[2].loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;
    load_cases[3].initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    load_cases[4].finalLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentDescription expected[5];
    memcpy(expected,load_cases,sizeof expected);
    expected[0].loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;
    assert(hades_vulkan_test_preserve_color_loads(load_cases,5)==1);
    assert(!memcmp(load_cases,expected,sizeof expected));
    assert(!hades_vulkan_test_preserve_color_loads(load_cases,5));
    load_cases[0].loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    VkRenderPassCreateInfo load_render_info=render_info;
    load_render_info.pAttachments=load_cases;
    VkRenderPass load_render_pass;
    assert(C(VULKAN_CREATE_RENDER_PASS,U(device),U(&load_render_info),0,U(&load_render_pass))==VK_SUCCESS);
    assert(load_cases[0].loadOp==VK_ATTACHMENT_LOAD_OP_DONT_CARE);
    VkFormat formats[]={VK_FORMAT_R8_UNORM,VK_FORMAT_R8G8B8A8_UNORM,
        VK_FORMAT_B8G8R8A8_UNORM,VK_FORMAT_BC7_UNORM_BLOCK};
    VkImage images[4];
    VkDeviceMemory storage[4];
    for (unsigned i=0;i<5;i++) {
        unsigned slot=i<4 ? i : 1;
        if (i==4) {
            assert(!hades_vulkan_test_capture_images(1));
            assert(hades_vulkan_test_capture_images(1)==-1);
        }
        VkImageCreateInfo info={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,
            .format=formats[slot],.extent={16,16,1},.mipLevels=1,.arrayLayers=i==2 ? 2 : 1,.samples=VK_SAMPLE_COUNT_1_BIT,
            .tiling=VK_IMAGE_TILING_OPTIMAL,.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT};
        if (slot==1)
            info.usage|=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (i<4) {
            assert(C(VULKAN_CREATE_IMAGE,U(device),U(&info),0,U(&images[slot]))==VK_SUCCESS);
            VkMemoryRequirements requirements;
            C(VULKAN_IMAGE_REQUIREMENTS,U(device),U(images[slot]),U(&requirements));
            unsigned type=0;
            while (type<memory->memoryTypeCount && !(requirements.memoryTypeBits&(1u<<type))) type++;
            assert(type<memory->memoryTypeCount);
            VkMemoryAllocateInfo allocation={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .allocationSize=requirements.size,.memoryTypeIndex=type};
            assert(C(VULKAN_ALLOCATE_MEMORY,U(device),U(&allocation),0,U(&storage[slot]))==VK_SUCCESS);
            assert(C(VULKAN_BIND_IMAGE_MEMORY,U(device),U(images[slot]),U(storage[slot]),0)==VK_SUCCESS);
        }
        if (!i) {
            memset(mapped,41,256);
        } else if (i<3 || i==4) {
            for (unsigned p=0;p<256;p++) {
                unsigned char *pixel=(unsigned char *)mapped+p*4;
                pixel[0]=i==4 ? 201 : 7;pixel[1]=i==4 ? 9 : 83;
                pixel[2]=i==4 ? 51 : 191;pixel[3]=i==4 ? 73 : i==1 ? 167 : 93;
            }
        } else {
            unsigned char block[16]={0x40};
            unsigned cursor=7;
            unsigned endpoints[8]={127,127,0,0,0,0,127,127};
            for (unsigned e=0;e<8;e++)
                for (unsigned b=0;b<7;b++,cursor++)
                    block[cursor/8]|=((endpoints[e]>>b)&1u)<<(cursor%8);
            for (unsigned p=0;p<2;p++,cursor++)
                block[cursor/8]|=1u<<(cursor%8);
            for (unsigned b=0;b<16;b++) memcpy((char *)mapped+b*16,block,16);
        }
        VkCommandPoolCreateInfo pool_info={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .queueFamilyIndex=family};
        VkCommandPool pool;
        assert(C(VULKAN_CREATE_COMMAND_POOL,U(device),U(&pool_info),0,U(&pool))==VK_SUCCESS);
        VkCommandBufferAllocateInfo commands={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
        VkCommandBuffer command;
        assert(C(VULKAN_ALLOCATE_COMMAND_BUFFERS,U(device),U(&commands),U(&command))==VK_SUCCESS);
        VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        assert(C(VULKAN_BEGIN_COMMAND_BUFFER,U(command),U(&begin))==VK_SUCCESS);
        VkImageMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .oldLayout=i==4 ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .srcAccessMask=i==4 ? VK_ACCESS_SHADER_READ_BIT : 0,
            .dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.image=images[slot],
            .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
        C(VULKAN_PIPELINE_BARRIER,U(command),i==4 ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,0,0,0,0,1,U(&barrier));
        VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={16,16,1}};
        C(VULKAN_COPY_BUFFER_IMAGE,U(command),U(source),U(images[slot]),VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,U(&copy));
        barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        C(VULKAN_PIPELINE_BARRIER,U(command),VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0,0,0,0,0,1,U(&barrier));
        assert(C(VULKAN_END_COMMAND_BUFFER,U(command))==VK_SUCCESS);
        VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};
        assert(C(VULKAN_QUEUE_SUBMIT,U(queue),1,U(&submit),0)==VK_SUCCESS);
        assert(C(VULKAN_QUEUE_IDLE,U(queue))==VK_SUCCESS);
        C(VULKAN_DESTROY_COMMAND_POOL,U(device),U(pool),0);
    }
    assert(!hades_vulkan_test_capture_images(3));
    VkImageViewCreateInfo view_info={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image=images[1],.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=formats[1],
        .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    VkImageView view;
    assert(C(VULKAN_CREATE_IMAGE_VIEW,U(device),U(&view_info),0,U(&view))==VK_SUCCESS);
    VkFramebufferCreateInfo framebuffer_info={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass=load_render_pass,.attachmentCount=1,.pAttachments=&view,.width=16,.height=16,.layers=1};
    VkFramebuffer framebuffer;
    assert(C(VULKAN_CREATE_FRAMEBUFFER,U(device),U(&framebuffer_info),0,U(&framebuffer))==VK_SUCCESS);
    VkCommandPoolCreateInfo load_pool_info={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=family};
    VkCommandPool load_pool;
    assert(C(VULKAN_CREATE_COMMAND_POOL,U(device),U(&load_pool_info),0,U(&load_pool))==VK_SUCCESS);
    VkCommandBufferAllocateInfo load_commands={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool=load_pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer load_command;
    assert(C(VULKAN_ALLOCATE_COMMAND_BUFFERS,U(device),U(&load_commands),U(&load_command))==VK_SUCCESS);
    VkCommandBufferBeginInfo load_begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    assert(C(VULKAN_BEGIN_COMMAND_BUFFER,U(load_command),U(&load_begin))==VK_SUCCESS);
    VkImageMemoryBarrier load_barrier={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,.newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcAccessMask=VK_ACCESS_SHADER_READ_BIT,.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=images[1],.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    C(VULKAN_PIPELINE_BARRIER,U(load_command),VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0,0,0,0,0,1,U(&load_barrier));
    VkRenderPassBeginInfo pass_begin={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass=load_render_pass,.framebuffer=framebuffer,.renderArea={{0,0},{16,16}}};
    C(VULKAN_BEGIN_RENDER_PASS,U(load_command),U(&pass_begin),VK_SUBPASS_CONTENTS_INLINE);
    C(VULKAN_END_RENDER_PASS,U(load_command));
    load_barrier.oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    load_barrier.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    load_barrier.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    load_barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    C(VULKAN_PIPELINE_BARRIER,U(load_command),VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,0,0,0,0,1,U(&load_barrier));
    load_barrier.oldLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    load_barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    load_barrier.srcAccessMask=VK_ACCESS_SHADER_READ_BIT;
    load_barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    C(VULKAN_PIPELINE_BARRIER,U(load_command),VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,0,0,0,0,1,U(&load_barrier));
    VkClearColorValue later_color={.float32={17.0f/255,29.0f/255,101.0f/255,113.0f/255}};
    vkCmdClearColorImage(load_command,images[1],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&later_color,1,&load_barrier.subresourceRange);
    load_barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    load_barrier.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    load_barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    load_barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    C(VULKAN_PIPELINE_BARRIER,U(load_command),VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,0,0,0,0,1,U(&load_barrier));
    assert(C(VULKAN_END_COMMAND_BUFFER,U(load_command))==VK_SUCCESS);
    VkSubmitInfo load_submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&load_command};
    assert(C(VULKAN_QUEUE_SUBMIT,U(queue),1,U(&load_submit),0)==VK_SUCCESS);
    assert(C(VULKAN_QUEUE_IDLE,U(queue))==VK_SUCCESS);
    assert(!hades_vulkan_test_capture_images(6));
    C(VULKAN_DESTROY_COMMAND_POOL,U(device),U(load_pool),0);
    C(VULKAN_DESTROY_FRAMEBUFFER,U(device),U(framebuffer),0);
    C(VULKAN_DESTROY_IMAGE_VIEW,U(device),U(view),0);
    C(VULKAN_DESTROY_RENDER_PASS,U(device),U(load_render_pass),0);
    for (unsigned i=0;i<4;i++) {
        C(VULKAN_DESTROY_IMAGE,U(device),U(images[i]),0);
        C(VULKAN_FREE_MEMORY,U(device),U(storage[i]),0);
    }
    assert(!hades_vulkan_test_capture_images(2));
    VkImage inventory[600];
    VkImageCreateInfo inventory_info={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType=VK_IMAGE_TYPE_2D,.format=VK_FORMAT_R8_UNORM,.extent={16,16,1},
        .mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,
        .tiling=VK_IMAGE_TILING_OPTIMAL,.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    for (unsigned i=0;i<600;i++)
        assert(C(VULKAN_CREATE_IMAGE,U(device),U(&inventory_info),0,U(&inventory[i]))==VK_SUCCESS);
    assert(!hades_vulkan_test_capture_images(4));
    for (unsigned i=0;i<600;i++)
        C(VULKAN_DESTROY_IMAGE,U(device),U(inventory[i]),0);
    assert(!hades_vulkan_test_capture_images(5));
    C(VULKAN_DESTROY_RENDER_PASS,U(device),U(render_pass),0);
    puts("VULKAN_TEXTURE_SAMPLE_OK: real uploads and GPU R8/RGBA/BGRA/BC7 samples, same-image reupload; destroyed images removed");
}

int main(int argc,char **argv) {
    assert(argc==3);
    assert(!hades_vulkan_test_texture_readback(argv[1]));
    assert(hades_vulkan_test_texture_readback(argv[1])==-1);
    assert(!hades_vulkan_configure_capture(argv[2],600));
    uint64_t tail[102]={0};
    tail[8]=0xDEADBEEF;
    tail[9]=0xBAD;
    tail[100]=3;
    tail[101]=U(&tail_barrier);
    assert(!hades_vulkan_test_invoke(HADES_HOST_VULKAN_PIPELINE_BARRIER,(PFN_vkVoidFunction)check_tail,tail));
    uint64_t null_set[102]={0};
    assert(hades_vulkan_test_invoke(HADES_HOST_VULKAN_UPDATE_DESCRIPTOR_TEMPLATE,NULL,null_set)==-1);
    const size_t mib=1024*1024;
    hades_memory_config policy={4*mib,4*mib,8*mib,0,0x1234,1ull<<39,1ull<<44,1,0,0};
    assert(!hades_memory_initialize(&policy));
    assert(!hades_memory_register(U(pool),sizeof pool,4,3,99));
    assert(!hades_vulkan_set_callback_runner(test_runner));
    assert(!hades_vulkan_test_surface_geometry());
    const char *pass_frame=getenv("HADES_VULKAN_PASS_CAPTURE_FRAME");
    int pass_enabled=pass_frame && !strcmp(pass_frame,"1");
    assert(hades_vulkan_test_pass_window(0,0)==pass_enabled);
    assert(!hades_vulkan_test_pass_window(0,1));
    assert(!hades_vulkan_test_pass_window(0,2));
    assert(!hades_vulkan_test_pass_window(1,1));
    assert(hades_vulkan_test_flow_window(0,0)==(pass_enabled ? 1u : 0u));
    assert(hades_vulkan_test_flow_window(1,1)==(pass_enabled ? 2u : 0u));
    assert(!hades_vulkan_test_flow_window(0,1));
    assert(!hades_vulkan_test_flow_window(1,2));
    assert(!hades_vulkan_test_flow_window(2,2));
    puts("PASS_WINDOW_OK: present entry closes recording even before present completion");
    static_load_probe(pass_enabled);
    assert(!C(GRAPHICS_INITIALIZE,U(pool),sizeof pool));
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.apiVersion=VK_API_VERSION_1_1};
    const char *extensions[]={"VK_EXT_debug_utils"};
    VkInstanceCreateInfo info={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app,
        .enabledExtensionCount=1,.ppEnabledExtensionNames=extensions};
    VkInstance instance;
    VkAllocationCallbacks callbacks={.pUserData=&allocator_counts,.pfnAllocation=test_allocate,
        .pfnReallocation=test_reallocate,.pfnFree=test_free};
    uint64_t callback_frame[102]={0,0,U(&callbacks),0};
    assert(!hades_vulkan_test_invoke(HADES_HOST_VULKAN_CREATE_BUFFER,(PFN_vkVoidFunction)check_allocator,callback_frame));
    assert(atomic_load(&allocator_counts.allocate)==2 && atomic_load(&allocator_counts.release)==2);
    assert(C(VULKAN_CREATE_INSTANCE,U(&info),U(&callbacks),U(&instance))==VK_SUCCESS);
    VkDebugUtilsMessengerCreateInfoEXT debug_info={.sType=VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
        .messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT,.pfnUserCallback=test_debug,
        .pUserData=&allocator_counts};
    VkDebugUtilsMessengerEXT messenger;
    assert(C(VULKAN_DEBUG_MESSENGER_CREATE,U(instance),U(&debug_info),U(&callbacks),U(&messenger))==VK_SUCCESS);
    VkDebugUtilsMessengerCallbackDataEXT notification={.sType=VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT,
        .pMessage="local typed guest callback probe"};
    C(VULKAN_DEBUG_SUBMIT,U(instance),VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT,U(&notification));
    assert(atomic_load(&allocator_counts.debug)>0);
    C(VULKAN_DEBUG_MESSENGER_DESTROY,U(instance),U(messenger),U(&callbacks));
    VkInstanceCreateInfo rejected=info;
    rejected.pNext=&tail_barrier;
    uint64_t invalid[102]={U(&rejected),0,U(&instance)};
    assert(hades_vulkan_call(HADES_HOST_VULKAN_CREATE_INSTANCE,"vkCreateInstance",invalid));
    uint64_t allocator[102]={0,0,1,0};
    assert(hades_vulkan_call(HADES_HOST_VULKAN_CREATE_BUFFER,"vkCreateBuffer",allocator));
    uint32_t count=1;
    VkPhysicalDevice physical;
    assert(C(VULKAN_PHYSICAL_DEVICES,U(instance),U(&count),U(&physical))==VK_SUCCESS && count==1);
    uint32_t families=0;
    C(VULKAN_QUEUE_PROPERTIES,U(physical),U(&families),0);
    assert(families && families<=32);
    VkQueueFamilyProperties properties[32];
    C(VULKAN_QUEUE_PROPERTIES,U(physical),U(&families),U(properties));
    unsigned family=0;
    while (family<families && !(properties[family].queueFlags&VK_QUEUE_GRAPHICS_BIT)) family++;
    assert(family<families);
    float priority=1;
    VkDeviceQueueCreateInfo queue_info={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex=family,.queueCount=1,.pQueuePriorities=&priority};
    VkDeviceCreateInfo device_info={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount=1,.pQueueCreateInfos=&queue_info};
    VkDevice device;
    assert(C(VULKAN_CREATE_DEVICE,U(physical),U(&device_info),U(&callbacks),U(&device))==VK_SUCCESS);
    VkQueue queue;
    C(VULKAN_DEVICE_QUEUE,U(device),family,0,U(&queue));
    VkPhysicalDeviceMemoryProperties memory;
    C(VULKAN_MEMORY_PROPERTIES,U(physical),U(&memory));
    VkBuffer buffers[2];
    VkDeviceMemory allocations[2];
    void *mapped[2];
    for (unsigned i=0;i<2;i++) {
        VkBufferCreateInfo buffer_info={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=1024,
            .usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            .sharingMode=VK_SHARING_MODE_EXCLUSIVE};
        assert(C(VULKAN_CREATE_BUFFER,U(device),U(&buffer_info),U(&callbacks),U(&buffers[i]))==VK_SUCCESS);
        VkMemoryRequirements requirements;
        C(VULKAN_BUFFER_REQUIREMENTS,U(device),U(buffers[i]),U(&requirements));
        unsigned type=0;
        for (;type<memory.memoryTypeCount;type++)
            if ((requirements.memoryTypeBits&(1u<<type))
                && (memory.memoryTypes[type].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
                   ==(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
                break;
        assert(type<memory.memoryTypeCount);
        VkMemoryAllocateInfo allocation={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize=requirements.size,.memoryTypeIndex=type};
        assert(C(VULKAN_ALLOCATE_MEMORY,U(device),U(&allocation),U(&callbacks),U(&allocations[i]))==VK_SUCCESS);
        assert(C(VULKAN_BIND_BUFFER_MEMORY,U(device),U(buffers[i]),U(allocations[i]),0)==VK_SUCCESS);
        assert(C(VULKAN_MAP_MEMORY,U(device),U(allocations[i]),0,VK_WHOLE_SIZE,0,U(&mapped[i]))==VK_SUCCESS);
        assert(hades_memory_writable(U(mapped[i]),1024));
        for (unsigned j=0;j<1024;j++) ((unsigned char *)mapped[i])[j]=i ? 0 : (unsigned char)(j*37+11);
    }
    descriptor_budget_probe(device,buffers[0],&callbacks);
    VkCommandPoolCreateInfo command_info={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=family};
    VkCommandPool command_pool;
    assert(C(VULKAN_CREATE_COMMAND_POOL,U(device),U(&command_info),0,U(&command_pool))==VK_SUCCESS);
    VkCommandBufferAllocateInfo allocate_commands={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool=command_pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer command;
    assert(C(VULKAN_ALLOCATE_COMMAND_BUFFERS,U(device),U(&allocate_commands),U(&command))==VK_SUCCESS);
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    assert(C(VULKAN_BEGIN_COMMAND_BUFFER,U(command),U(&begin))==VK_SUCCESS);
    VkBufferCopy copy={.size=1024};
    C(VULKAN_COPY_BUFFER,U(command),U(buffers[0]),U(buffers[1]),1,U(&copy));
    VkMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
    C(VULKAN_PIPELINE_BARRIER,U(command),VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,
        1,U(&barrier),0,0,0,0);
    assert(C(VULKAN_END_COMMAND_BUFFER,U(command))==VK_SUCCESS);
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&command};
    assert(C(VULKAN_QUEUE_SUBMIT,U(queue),1,U(&submit),0)==VK_SUCCESS);
    assert(C(VULKAN_QUEUE_IDLE,U(queue))==VK_SUCCESS);
    assert(!memcmp(mapped[0],mapped[1],1024));
    texture_sample_probe(device,queue,family,&memory,buffers[0],mapped[0]);
    for (unsigned i=0;i<2;i++) {
        uintptr_t old=U(mapped[i]);
        C(VULKAN_UNMAP_MEMORY,U(device),U(allocations[i]));
        assert(!hades_memory_writable(old,1024));
        C(VULKAN_DESTROY_BUFFER,U(device),U(buffers[i]),U(&callbacks));
        uint64_t mismatch[102]={U(device),U(allocations[i]),0};
        assert(hades_vulkan_call(HADES_HOST_VULKAN_FREE_MEMORY,"vkFreeMemory",mismatch));
        C(VULKAN_FREE_MEMORY,U(device),U(allocations[i]),U(&callbacks));
        uint64_t frame[102]={U(device),U(allocations[i]),0};
        assert(hades_vulkan_call(HADES_HOST_VULKAN_FREE_MEMORY,"vkFreeMemory",frame));
    }
    C(VULKAN_DESTROY_COMMAND_POOL,U(device),U(command_pool),0);
    C(VULKAN_DESTROY_DEVICE,U(device),U(&callbacks));
    C(VULKAN_DESTROY_INSTANCE,U(instance),U(&callbacks));
    assert(atomic_load(&allocator_counts.allocate)>0);
    assert(atomic_load(&allocator_counts.allocate)==atomic_load(&allocator_counts.release));
    assert(!hades_memory_shutdown());
    puts("VULKAN_BUFFER_GPU_OK: typed AOT calls, real Metal queue copy/readback, owned mapping revocation; not a game frame");
    return 0;
}
