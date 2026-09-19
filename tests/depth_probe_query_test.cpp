// Deterministic production-reader tests: no graphics device or pixel buffers.
// MINGW32: g++ -std=c++20 -O2 -Wall -Wextra -DWIN32_LEAN_AND_MEAN -DNOMINMAX
// -municode -static -Ibuild/generated tests/depth_probe_query_test.cpp
// src/grayscale_d3d9.cpp src/motion_d3d9.cpp -ld3dcompiler_47
// -o build/depth_probe_query_test.exe
#include "../src/grayscale_d3d9.hpp"
#include <cstdio>
#include <cstdlib>
#include <array>

namespace
{
    void require_probe(bool value,const char *message)
    {
        if(!value){std::fprintf(stderr,"FAIL: %s\n",message);std::exit(1);}
    }

    class mock_query final : public IDirect3DQuery9
    {
    public:
        HRESULT response=S_OK;
        DWORD count=0;
        unsigned calls=0;
        ULONG references=1;
        HRESULT WINAPI QueryInterface(REFIID,void **object) override
        {if(object)*object=nullptr;return E_NOINTERFACE;}
        ULONG WINAPI AddRef() override {return ++references;}
        ULONG WINAPI Release() override {require_probe(references>0,"query reference underflow");return --references;}
        HRESULT WINAPI GetDevice(IDirect3DDevice9 **device) override
        {if(device)*device=nullptr;return E_NOTIMPL;}
        D3DQUERYTYPE WINAPI GetType() override {return D3DQUERYTYPE_OCCLUSION;}
        DWORD WINAPI GetDataSize() override {return sizeof(DWORD);}
        HRESULT WINAPI Issue(DWORD) override
        {require_probe(false,"result reader issued a GPU operation");return E_FAIL;}
        HRESULT WINAPI GetData(void *data,DWORD size,DWORD flags) override
        {
            require_probe(data!=nullptr&&size==sizeof(DWORD),"reader requested anything other than one scalar DWORD");
            require_probe(flags==0,"reader requested a GPU flush");
            ++calls;
            // Deliberately write even on S_FALSE/error: those bytes have no
            // validity guarantee and must never escape the production reader.
            *static_cast<DWORD *>(data)=count;
            return response;
        }
    };

    struct fixture
    {
        mock_query storage[3];
        IDirect3DQuery9 *queries[3]={&storage[0],&storage[1],&storage[2]};
        fixture(){storage[0].count=256;storage[1].count=160;storage[2].count=64;}
        enr::depth_probe_result poll(std::uint64_t generation=17)
        {return enr::read_depth_probe_queries(queries,generation);}
    };

    void require_discarded(const enr::depth_probe_result &result,enr::depth_probe_result::state state)
    {
        require_probe(result.status==state,"incorrect incomplete/invalid result status");
        require_probe(result.valid==0&&result.signature1==0&&result.signature2==0&&result.generation==0,
            "partial, invalid, or stale numeric result escaped");
    }
}

int wmain()
{
    static_assert(sizeof(void *)==4);
    static_assert(enr::depth_probe_sample_count==256);
    unsigned cases=0;
    {
        fixture f;const auto result=f.poll();
        require_probe(result.status==enr::depth_probe_result::state::ready&&result.valid==256&&
            result.signature1==160&&result.signature2==64&&result.generation==17,"valid triplet not published correctly");
        for(const auto &query:f.storage)require_probe(query.calls==1&&query.references==1,"reader changed ownership or repeated polling");
        ++cases;
    }
    {
        fixture f;for(auto &q:f.storage)q.count=0;
        const auto result=f.poll();
        require_probe(result.status==enr::depth_probe_result::state::ready&&result.valid==0,
            "valid all-clear scene was rejected");++cases;
    }
    for(unsigned index=0;index<3;++index)
    {
        fixture f;f.storage[index].response=S_FALSE;f.storage[index].count=0xffffffff;
        require_discarded(f.poll(),enr::depth_probe_result::state::none);
        for(unsigned q=0;q<3;++q)require_probe(f.storage[q].calls==(q<=index?1u:0u),"pending query caused extra polling");
        // A later poll must collect a fresh, complete triplet and generation.
        f.storage[index].response=S_OK;
        f.storage[0].count=128;f.storage[1].count=75;f.storage[2].count=39;
        const auto fresh=f.poll(18);
        require_probe(fresh.status==enr::depth_probe_result::state::ready&&fresh.valid==128&&
            fresh.signature1==75&&fresh.signature2==39&&fresh.generation==18,
            "pending poll reused stale partial values or generation");++cases;
    }
    for(unsigned index=0;index<3;++index)
        for(const DWORD impossible:{257u,65535u,0xffffffffu})
        {
            fixture f;f.storage[index].count=impossible;
            const auto result=f.poll();require_discarded(result,enr::depth_probe_result::state::invalid);
            require_probe(result.error==E_UNEXPECTED,"out-of-range query result lacks explicit error");++cases;
        }
    for(unsigned index=1;index<3;++index)
    {
        fixture f;f.storage[0].count=100;f.storage[1].count=50;f.storage[2].count=25;f.storage[index].count=101;
        const auto result=f.poll();require_discarded(result,enr::depth_probe_result::state::invalid);
        require_probe(result.error==E_UNEXPECTED,"signature larger than valid coverage was accepted");++cases;
    }
    for(const HRESULT failure:{E_FAIL,D3DERR_DEVICELOST,D3DERR_DEVICENOTRESET,D3DERR_INVALIDCALL})
        for(unsigned index=0;index<3;++index)
        {
            fixture f;f.storage[index].response=failure;f.storage[index].count=0xffffffff;
            const auto result=f.poll();require_discarded(result,enr::depth_probe_result::state::invalid);
            require_probe(result.error==failure,"driver failure HRESULT not preserved");
            for(unsigned q=0;q<3;++q)require_probe(f.storage[q].calls==(q<=index?1u:0u),"error caused extra query calls");
            ++cases;
        }
    for(unsigned index=0;index<3;++index)
    {
        fixture f;f.storage[index].response=static_cast<HRESULT>(2);f.storage[index].count=123;
        const auto result=f.poll();require_discarded(result,enr::depth_probe_result::state::invalid);
        require_probe(result.error==E_UNEXPECTED,"successful non-S_OK status accepted undefined bytes");++cases;
    }
    for(unsigned index=0;index<3;++index)
    {
        fixture f;f.queries[index]=nullptr;
        const auto result=f.poll();require_discarded(result,enr::depth_probe_result::state::invalid);
        require_probe(result.error==E_POINTER,"missing query not rejected safely");++cases;
    }
    std::printf("PASS: %u deterministic production probe-reader cases: S_FALSE, partial/stale generations, impossible counts, "
        "signature bounds, failed/device-lost/unexpected HRESULTs, null queries, scalar-only flags-zero polling, unchanged ownership.\n",cases);
    return 0;
}
