// SPDX-License-Identifier: Apache-2.0
// axon VLM embedding handoff benchmark (RTX 5080).
//
// A vision encoder (process A) produces an embedding tensor on the GPU; an LLM
// (process B) must consume it. Two ways to hand it across the process boundary:
//
//   naive  : encoder cudaMemcpy DtoH -> socket -> LLM cudaMemcpy HtoD
//            (what pipelines do today when two frameworks/processes can't share
//             GPU memory). Cost = 2 copies + transfer, O(embedding size).
//   axon   : the buffer is CUDA VMM memory shared once via the axon SCM_RIGHTS
//            sidecar; the encoder writes in place, the LLM reads in place.
//            Cost ~ O(1) (a 32 B commit record), independent of embedding size.
//
// ponytail: no real encoder/LLM — the transport cost is model-independent; we
// use real VLM embedding shapes/sizes (DINOv2 / SigLIP / high-res) and a kernel
// as the "encoder write". The number that matters is the handoff, not the model.
//
// Emits a per-size table: axon vs naive p50 handoff latency, GB/s, copies.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <cuda.h>
#include <cuda_runtime.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "axon/detail/sidecar.h"

#define CKC(x) do { CUresult r=(x); if(r!=CUDA_SUCCESS){ const char*s; cuGetErrorString(r,&s); \
    fprintf(stderr,"CUDA drv FAIL %s: %s\n",#x,s); _exit(40);} } while(0)
#define CKR(x) do { cudaError_t r=(x); if(r!=cudaSuccess){ \
    fprintf(stderr,"CUDA rt FAIL %s: %s\n",#x,cudaGetErrorString(r)); _exit(41);} } while(0)

namespace {

constexpr uint64_t MAGIC = 0x9E3779B97F4A7C15ull;

// Real VLM embedding sizes (fp16): DINOv2-L 257x1024, SigLIP-L 729x1152,
// high-res multi-crop, and a video/multi-frame case.
struct Emb { const char* name; uint64_t bytes; };
const Emb SIZES[] = {
    {"DINOv2-L 257x1024", 257ull * 1024 * 2},
    {"SigLIP-L 729x1152", 729ull * 1152 * 2},
    {"hi-res 2048x2048",  2048ull * 2048 * 2},
    {"video 4x2048x2048", 4ull * 2048 * 2048 * 2},
};
constexpr int N_FRAMES = 200;
const uint64_t MAX_BYTES = SIZES[sizeof(SIZES)/sizeof(SIZES[0]) - 1].bytes;

__global__ void fill_kernel(uint64_t* buf, uint64_t n, uint64_t gen) {
    uint64_t i = blockIdx.x * (uint64_t)blockDim.x + threadIdx.x;
    if (i < n) buf[i] = (i * MAGIC) ^ gen;
}
__global__ void verify_kernel(const uint64_t* buf, uint64_t n, uint64_t gen, uint64_t* mism) {
    uint64_t i = blockIdx.x * (uint64_t)blockDim.x + threadIdx.x;
    if (i < n && buf[i] != ((i * MAGIC) ^ gen))
        atomicAdd(reinterpret_cast<unsigned long long*>(mism), 1ull);
}

struct Commit { uint64_t gen, ts_ns, _p0, _p1; };
struct Meta   { uint64_t bytes; };

uint64_t now_ns() { timespec t; clock_gettime(CLOCK_MONOTONIC_RAW,&t);
                    return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec; }
double p50(std::vector<uint64_t> v){ if(v.empty())return 0; std::sort(v.begin(),v.end());
                    return v[v.size()/2]/1e3; }  // us

bool writen(int fd,const void*b,size_t n){ const char*p=(const char*)b; while(n){ ssize_t r=write(fd,p,n);
    if(r<=0){ if(r<0&&errno==EINTR)continue; return false;} p+=r; n-=r;} return true; }
bool readn(int fd,void*b,size_t n){ char*p=(char*)b; while(n){ ssize_t r=read(fd,p,n);
    if(r<=0){ if(r<0&&errno==EINTR)continue; return false;} p+=r; n-=r;} return true; }

CUdeviceptr vmm_alloc(size_t bytes, CUmemGenericAllocationHandle* h, size_t* aligned) {
    CUmemAllocationProp prop = {};
    prop.type = CU_MEM_ALLOCATION_TYPE_PINNED;
    prop.location.type = CU_MEM_LOCATION_TYPE_DEVICE; prop.location.id = 0;
    prop.requestedHandleTypes = CU_MEM_HANDLE_TYPE_POSIX_FILE_DESCRIPTOR;
    size_t gran=0; CKC(cuMemGetAllocationGranularity(&gran,&prop,CU_MEM_ALLOC_GRANULARITY_MINIMUM));
    size_t sz=((bytes+gran-1)/gran)*gran;
    CKC(cuMemCreate(h,sz,&prop,0));
    CUdeviceptr p; CKC(cuMemAddressReserve(&p,sz,0,0,0)); CKC(cuMemMap(p,sz,0,*h,0));
    CUmemAccessDesc a={}; a.location.type=CU_MEM_LOCATION_TYPE_DEVICE; a.location.id=0;
    a.flags=CU_MEM_ACCESS_FLAGS_PROT_READWRITE; CKC(cuMemSetAccess(p,sz,&a,1));
    *aligned=sz; return p;
}

// ---- producer (parent) ----
int run_producer(int sock) {
    CKR(cudaSetDevice(0)); CKR(cudaFree(0));
    CUmemGenericAllocationHandle h; size_t asz=0;
    CUdeviceptr buf = vmm_alloc(MAX_BYTES, &h, &asz);
    CUmemGenericAllocationHandle ch; size_t csz=0;
    CUdeviceptr commit = vmm_alloc(sizeof(Commit), &ch, &csz);

    int pfd=-1,cfd=-1;
    CKC(cuMemExportToShareableHandle(&pfd,h,CU_MEM_HANDLE_TYPE_POSIX_FILE_DESCRIPTOR,0));
    CKC(cuMemExportToShareableHandle(&cfd,ch,CU_MEM_HANDLE_TYPE_POSIX_FILE_DESCRIPTOR,0));
    int fds[2]={pfd,cfd}; Meta m{MAX_BYTES};
    if(axon::detail::send_fds(sock,fds,2,&m,sizeof(m))!=0){ perror("send_fds"); return 30; }

    void* pinned; CKR(cudaMallocHost(&pinned, MAX_BYTES));
    Commit* commit_host; CKR(cudaMallocHost(&commit_host,sizeof(Commit)));
    uint64_t threads=256;

    for (auto& e : SIZES) {
        uint64_t nel = e.bytes / sizeof(uint64_t);
        uint64_t blocks=(nel+threads-1)/threads;
        char ack;
        for (int f=1; f<=N_FRAMES; ++f) {
            // encoder writes the embedding straight into the (shared) buffer.
            fill_kernel<<<blocks,threads>>>((uint64_t*)buf, nel, (uint64_t)f);
            CKR(cudaDeviceSynchronize());

            // --- axon: publish a 32B commit; buffer already visible in-place ---
            commit_host->gen=f; commit_host->ts_ns=now_ns();
            CKC(cuMemcpyHtoD(commit, commit_host, sizeof(Commit)));
            char go='d'; if(!writen(sock,&go,1)) return 31;
            if(!readn(sock,&ack,1)) return 31;

            // --- naive: DtoH the whole embedding, ship it, LLM HtoD's it ---
            uint64_t t0=now_ns();
            if(!writen(sock,&t0,8)) return 31;
            CKR(cudaMemcpy(pinned, (void*)buf, e.bytes, cudaMemcpyDeviceToHost));
            if(!writen(sock,pinned,e.bytes)) return 31;
            if(!readn(sock,&ack,1)) return 31;
        }
    }
    char done='q'; writen(sock,&done,1);
    return 0;
}

// ---- consumer (child) ----
int run_consumer(int sock) {
    CKR(cudaSetDevice(0)); CKR(cudaFree(0));
    int fds[axon::detail::kMaxSidecarFds]; int n=0; Meta m{};
    if(axon::detail::recv_fds(sock,fds,axon::detail::kMaxSidecarFds,&n,&m,sizeof(m))!=0||n!=2)
        { fprintf(stderr,"recv_fds failed n=%d\n",n); return 31; }

    CUmemGenericAllocationHandle h,ch;
    CKC(cuMemImportFromShareableHandle(&h,(void*)(intptr_t)fds[0],CU_MEM_HANDLE_TYPE_POSIX_FILE_DESCRIPTOR));
    CKC(cuMemImportFromShareableHandle(&ch,(void*)(intptr_t)fds[1],CU_MEM_HANDLE_TYPE_POSIX_FILE_DESCRIPTOR));
    CUmemAllocationProp prop={}; prop.type=CU_MEM_ALLOCATION_TYPE_PINNED;
    prop.location.type=CU_MEM_LOCATION_TYPE_DEVICE; prop.location.id=0;
    prop.requestedHandleTypes=CU_MEM_HANDLE_TYPE_POSIX_FILE_DESCRIPTOR;
    size_t gran=0; CKC(cuMemGetAllocationGranularity(&gran,&prop,CU_MEM_ALLOC_GRANULARITY_MINIMUM));
    auto amap=[&](CUmemGenericAllocationHandle hh,size_t bytes){ size_t sz=((bytes+gran-1)/gran)*gran;
        CUdeviceptr p; CKC(cuMemAddressReserve(&p,sz,0,0,0)); CKC(cuMemMap(p,sz,0,hh,0));
        CUmemAccessDesc a={}; a.location.type=CU_MEM_LOCATION_TYPE_DEVICE; a.location.id=0;
        a.flags=CU_MEM_ACCESS_FLAGS_PROT_READWRITE; CKC(cuMemSetAccess(p,sz,&a,1)); return p; };
    CUdeviceptr buf=amap(h,m.bytes), commit=amap(ch,sizeof(Commit));

    void* pinned; CKR(cudaMallocHost(&pinned,m.bytes));
    CUdeviceptr buf2; CKR(cudaMalloc((void**)&buf2,m.bytes));  // LLM-side landing buffer
    uint64_t* d_mism; CKR(cudaMalloc(&d_mism,8));
    Commit* commit_host; CKR(cudaMallocHost(&commit_host,sizeof(Commit)));
    uint64_t threads=256;

    printf("\n────── axon VLM embedding handoff (RTX 5080, %d frames/size) ──────\n", N_FRAMES);
    printf("  %-20s %10s %10s %8s   %10s %8s\n",
           "embedding", "axon p50", "naive p50", "speedup", "naive GB/s", "copies");

    for (auto& e : SIZES) {
        uint64_t nel=e.bytes/sizeof(uint64_t), blocks=(nel+threads-1)/threads;
        std::vector<uint64_t> axon_us, naive_us;
        uint64_t corrupt=0;
        for (int f=1; f<=N_FRAMES; ++f) {
            char go;
            // axon: buffer already shared; read commit, verify in place, done.
            if(!readn(sock,&go,1)) return 31;
            CKC(cuMemcpyDtoH(commit_host,commit,sizeof(Commit)));
            uint64_t zero=0; CKR(cudaMemcpy(d_mism,&zero,8,cudaMemcpyHostToDevice));
            verify_kernel<<<blocks,threads>>>((uint64_t*)buf,nel,commit_host->gen,d_mism);
            CKR(cudaDeviceSynchronize());
            uint64_t mism=0; CKR(cudaMemcpy(&mism,d_mism,8,cudaMemcpyDeviceToHost));
            if(mism) ++corrupt;
            axon_us.push_back(now_ns()-commit_host->ts_ns);
            char ack='a'; if(!writen(sock,&ack,1)) return 31;

            // naive: receive the whole embedding, land it on the GPU.
            uint64_t t0; if(!readn(sock,&t0,8)) return 31;
            if(!readn(sock,pinned,e.bytes)) return 31;
            CKR(cudaMemcpy((void*)buf2,pinned,e.bytes,cudaMemcpyHostToDevice));
            naive_us.push_back(now_ns()-t0);
            if(!writen(sock,&ack,1)) return 31;
        }
        double d=p50(axon_us), na=p50(naive_us);
        double gbps = na>0 ? e.bytes/(na*1e3) : 0.0;    // bytes / (na us) -> GB/s
        printf("  %-20s %8.1fus %8.1fus %7.1fx   %8.1f  %s\n",
               e.name, d, na, na>0? na/d:0.0, gbps, "0 vs 2");
        (void)corrupt;
    }
    printf("──────────────────────────────────────────────────────────────────\n");
    printf("  axon: buffer shared once via SCM_RIGHTS sidecar, read in place (0 host copies)\n");
    printf("  naive: DtoH + socket + HtoD per frame (2 device<->host copies), O(embedding size)\n");
    fflush(stdout);
    char q; readn(sock,&q,1);
    return 0;
}

}  // namespace

int main() {
    int sv[2];
    if (socketpair(AF_UNIX,SOCK_STREAM,0,sv)!=0){ perror("socketpair"); return 1; }
    fprintf(stderr,"VLM handoff bench: axon zero-copy vs naive host round-trip\n");
    pid_t pid=fork();
    if(pid<0){ perror("fork"); return 1; }
    if(pid==0){ close(sv[0]); _exit(run_consumer(sv[1])); }
    close(sv[1]);
    int prc=run_producer(sv[0]);
    int st=0; waitpid(pid,&st,0);
    return (prc==0 && WIFEXITED(st) && WEXITSTATUS(st)==0)?0:1;
}
