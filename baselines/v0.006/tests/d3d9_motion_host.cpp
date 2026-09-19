// GPU-only correspondence regression. No image map/readback or query flush.
// g++ -std=c++20 -O2 -Wall -Wextra -DWIN32_LEAN_AND_MEAN -DNOMINMAX -municode
// -static -Ibuild/generated tests/d3d9_motion_host.cpp src/grayscale_d3d9.cpp src/motion_d3d9.cpp
// -ld3dcompiler_47 -o build/d3d9_motion_host.exe
#define ENR_TEMPORAL_FIXTURE_HELPERS_ONLY
#include "d3d9_temporal_host.cpp"

namespace
{
    bool compile_fixture_shader(IDirect3DDevice9 *device, const char *source,
        IDirect3DPixelShader9 **shader, const char *name)
    {
        com_ptr<ID3DBlob> bytecode, errors;
        if (!succeeded(D3DCompile(source, std::strlen(source), name, nullptr, nullptr,
                "main", "ps_3_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, bytecode.put(), errors.put()), name))
        {
            if (errors) std::fprintf(stderr, "%s\n", static_cast<const char *>(errors->GetBufferPointer()));
            return false;
        }
        return succeeded(device->CreatePixelShader(static_cast<const DWORD *>(bytecode->GetBufferPointer()), shader), name);
    }

    bool fixture_quad(IDirect3DDevice9 *device, UINT width, UINT height, IDirect3DVertexBuffer9 **vertices)
    {
        if (!succeeded(device->CreateVertexBuffer(4 * sizeof(quad_vertex), D3DUSAGE_WRITEONLY,
                D3DFVF_XYZRHW | D3DFVF_TEX1, D3DPOOL_MANAGED, vertices, nullptr), "CreateVertexBuffer(motion fixture)")) return false;
        const quad_vertex quad[] = {{-.5f,-.5f,0,1,0,0}, {width-.5f,-.5f,0,1,1,0},
            {-.5f,height-.5f,0,1,0,1}, {width-.5f,height-.5f,0,1,1,1}};
        void *upload = nullptr;
        if (!succeeded((*vertices)->Lock(0, sizeof(quad), &upload, 0), "Lock(vertex upload)")) return false;
        std::memcpy(upload, quad, sizeof(quad));
        return succeeded((*vertices)->Unlock(), "Unlock(vertex upload)");
    }

    bool fixture_state(IDirect3DDevice9 *device, IDirect3DSurface9 *target,
        IDirect3DPixelShader9 *shader, IDirect3DVertexBuffer9 *vertices, UINT width, UINT height)
    {
        const D3DVIEWPORT9 viewport {0, 0, width, height, 0, 1};
        if (!succeeded(device->SetTexture(0, nullptr), "Unbind fixture input") ||
            !succeeded(device->SetDepthStencilSurface(nullptr), "Unbind fixture depth") ||
            !succeeded(device->SetRenderTarget(0, target), "SetRenderTarget(fixture)") ||
            !succeeded(device->SetViewport(&viewport), "SetViewport(fixture)") ||
            !succeeded(device->SetPixelShader(shader), "SetPixelShader(fixture)") ||
            !succeeded(device->SetVertexShader(nullptr), "SetVertexShader(fixture)") ||
            !succeeded(device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1), "SetFVF(fixture)") ||
            !succeeded(device->SetStreamSource(0, vertices, 0, sizeof(quad_vertex)), "SetStreamSource(fixture)")) return false;
        for (const auto state : {render_state{D3DRS_ZENABLE,FALSE}, {D3DRS_ZWRITEENABLE,FALSE},
                {D3DRS_STENCILENABLE,FALSE}, {D3DRS_ALPHATESTENABLE,FALSE}, {D3DRS_ALPHABLENDENABLE,FALSE},
                {D3DRS_SCISSORTESTENABLE,FALSE}, {D3DRS_CULLMODE,D3DCULL_NONE}, {D3DRS_COLORWRITEENABLE,15},
                {D3DRS_SRGBWRITEENABLE,FALSE}, {D3DRS_FOGENABLE,FALSE}, {D3DRS_DITHERENABLE,FALSE},
                {D3DRS_CLIPPLANEENABLE,0}, {D3DRS_MULTISAMPLEMASK,0xffffffff}})
            if (!succeeded(device->SetRenderState(state.type, state.value), "SetRenderState(fixture)")) return false;
        for (const auto state : {D3DSAMP_MINFILTER,D3DSAMP_MAGFILTER})
            if (!succeeded(device->SetSamplerState(0, state, D3DTEXF_POINT), "SetSamplerState(fixture)")) return false;
        return succeeded(device->SetSamplerState(0,D3DSAMP_MIPFILTER,D3DTEXF_NONE), "SetSamplerState(mip)") &&
            succeeded(device->SetSamplerState(0,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP), "SetSamplerState(u)") &&
            succeeded(device->SetSamplerState(0,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP), "SetSamplerState(v)") &&
            succeeded(device->SetSamplerState(0,D3DSAMP_SRGBTEXTURE,FALSE), "SetSamplerState(sRGB)");
    }

    struct procedural_scene
    {
        IDirect3DDevice9 *device;
        com_ptr<IDirect3DPixelShader9> shader;
        IDirect3DVertexBuffer9 *vertices = nullptr;
        UINT width = 0, height = 0;
        explicit procedural_scene(IDirect3DDevice9 *value) : device(value) {}
        ~procedural_scene() { drop(vertices); }
        bool initialize(UINT w, UINT h)
        {
            width = w; height = h; drop(vertices);
            constexpr char source[] = R"(
float4 image : register(c0);
float4 options : register(c1);
float4 rotation : register(c2);
float hash(float2 p) { return frac(sin(dot(p,float2(127.1,311.7))) * 43758.5453); }
float noise(float2 p) {
    float2 i=floor(p), f=frac(p); f=f*f*(3-2*f);
    return lerp(lerp(hash(i),hash(i+float2(1,0)),f.x),
        lerp(hash(i+float2(0,1)),hash(i+1),f.x),f.y);
}
float4 main(float2 uv:TEXCOORD0):COLOR0 {
    float2 shift=(options.x>1.5 && uv.x>.5) ? options.yz : image.zw;
    float2 p=uv*image.xy-shift;
    float2 centered=p-image.xy*.5;
    p=float2(rotation.x*centered.x+rotation.y*centered.y,
        -rotation.y*centered.x+rotation.x*centered.y)+image.xy*.5;
    float value=.08+.62*noise(p/11)+.25*noise(p/5.3+float2(87,31));
    if(options.x>.5 && options.x<1.5) value=.02;
    return float4(value,value,value,options.w);
})";
            return (shader || compile_fixture_shader(device, source, shader.put(), "Compile(procedural scene)")) &&
                fixture_quad(device,w,h,&vertices);
        }
        bool draw(IDirect3DSurface9 *target, float x, float y, unsigned mode=0, float other_x=0, float other_y=0,float angle=0)
        {
            const float constants[] = {static_cast<float>(width),static_cast<float>(height),x,y,
                static_cast<float>(mode),other_x,other_y,64.0f/255,std::cos(angle),std::sin(angle),0,0};
            return fixture_state(device,target,shader,vertices,width,height) &&
                succeeded(device->SetPixelShaderConstantF(0,constants,3), "SetConstants(scene)") &&
                succeeded(device->DrawPrimitive(D3DPT_TRIANGLESTRIP,0,2), "Draw(scene)");
        }
    };

    struct motion_predicate
    {
        IDirect3DDevice9 *device;
        com_ptr<IDirect3DPixelShader9> shader;
        com_ptr<IDirect3DSurface9> target;
        com_ptr<IDirect3DVertexBuffer9> vertices;
        struct pending { IDirect3DQuery9 *query; std::string label; DWORD minimum; DWORD maximum; };
        std::vector<pending> queries;
        explicit motion_predicate(IDirect3DDevice9 *value) : device(value) {}
        ~motion_predicate() { for(auto &q:queries) q.query->Release(); }
        bool initialize()
        {
            constexpr char source[] = R"(
sampler2D image:register(s0);
float4 expected:register(c0);
float4 region:register(c1);
float4 options:register(c2);
float4 rotation:register(c3);
float4 main(float2 uv:TEXCOORD0):COLOR0 {
    float2 sample_uv=lerp(region.xy,region.zw,uv);
    float4 value=tex2D(image,sample_uv);
    float passed;
    if(options.x>7.5)
        passed=(max(max(value.r,value.g),value.b)<.00001);
    else if(options.x>6.5)
        passed=(max(max(value.r,value.g),value.b)-min(min(value.r,value.g),value.b)>.02);
    else if(options.x>5.5) {
        float2 center=(floor(sample_uv*ceil(rotation.xy/8))+.5)/ceil(rotation.xy/8)*rotation.xy;
        float2 local=center-rotation.xy*.5;
        float2 transformed=float2(rotation.z*local.x+rotation.w*local.y,-rotation.w*local.x+rotation.z*local.y);
        float2 wanted=transformed-local;
        passed=(value.b>.001 && all(abs(value.rg-wanted)<=options.y));
    }
    else if(options.x>.5 && options.x<1.5)
        passed=(value.b<.00001 && abs(value.r)+abs(value.g)<.001 && value.a>.99);
    else if(options.x>1.5 && options.x<2.5)
        passed=abs(value.a-expected.w)<.00001;
    else if(options.x>4.5)
        passed=(value.b>.001 && (value.r*expected.x<0 || value.g*expected.y<0));
    else if(options.x>3.5)
        passed=(value.b>.001);
    else if(options.x>2.5)
        passed=(max(max(value.r,value.g),value.b)>.01);
    else
        passed=(value.a>.5 && value.b>=expected.z &&
            abs(value.r-expected.x)<=options.y && abs(value.g-expected.y)<=options.y);
    clip(passed ? 1 : -1); return 1;
})";
            return compile_fixture_shader(device,source,shader.put(),"Compile(motion predicate)") &&
                succeeded(device->CreateRenderTarget(8,8,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,target.put(),nullptr),"CreateRenderTarget(motion predicate)") &&
                fixture_quad(device,8,8,vertices.put());
        }
        bool add(IDirect3DTexture9 *input, const char *label, float x, float y,
            unsigned mode=0, DWORD minimum=56, float left=.25f, float top=.25f,
            float right=.75f, float bottom=.75f, float tolerance=1.01f,float angle=0)
        {
            if (!require_temporal(input!=nullptr,"predicate input texture missing")) return false;
            IDirect3DQuery9 *query=nullptr;
            if(!succeeded(device->CreateQuery(D3DQUERYTYPE_OCCLUSION,&query),"CreateQuery(motion predicate)"))return false;
            queries.push_back({query,label,minimum,64});
            const float constants[]={x,y,.001f,64.0f/255,left,top,right,bottom,
                static_cast<float>(mode),tolerance,0,0,256,192,std::cos(angle),std::sin(angle)};
            return fixture_state(device,target,shader,vertices,8,8) &&
                succeeded(device->SetRenderState(D3DRS_COLORWRITEENABLE,0),"Mask predicate output") &&
                succeeded(device->SetTexture(0,input),"Bind predicate input") &&
                succeeded(device->SetPixelShaderConstantF(0,constants,4),"SetConstants(predicate)") &&
                succeeded(query->Issue(D3DISSUE_BEGIN),"Issue(predicate begin)") &&
                succeeded(device->DrawPrimitive(D3DPT_TRIANGLESTRIP,0,2),"Draw(predicate)") &&
                succeeded(query->Issue(D3DISSUE_END),"Issue(predicate end)") &&
                succeeded(device->SetTexture(0,nullptr),"Unbind predicate input");
        }
        bool resolve()
        {
            const ULONGLONG started=GetTickCount64();
            unsigned remaining=static_cast<unsigned>(queries.size());
            std::vector<bool> completed(queries.size(),false);
            bool pass=true;
            while(remaining && GetTickCount64()-started<10000)
            {
                for(unsigned i=0;i<queries.size();++i)
                {
                    if(completed[i])continue;
                    DWORD count=0;
                    const HRESULT result=queries[i].query->GetData(&count,sizeof(count),0);
                    if(result==S_FALSE)continue;
                    if(!succeeded(result,"GetData(nonblocking scalar predicate)"))return false;
                    const auto &q=queries[i];
                    const bool good=count>=q.minimum && count<=q.maximum;
                    std::printf("%s: %s: %lu/64 GPU samples (required %lu..%lu)\n",good?"PASS":"FAIL",q.label.c_str(),count,q.minimum,q.maximum);
                    pass &= good; completed[i]=true;--remaining;
                }
                if(remaining)
                {
                    if(!succeeded(device->Present(nullptr,nullptr,nullptr,nullptr),"Present(query progress)"))return false;
                    Sleep(10); // Pace the standalone CPU fixture; never flush a query or wait on the GPU.
                }
            }
            return pass && require_temporal(remaining==0,"asynchronous motion predicates timed out");
        }
    };

    // Optional, standalone-fixture GPU timing. Query storage is created once;
    // only scalar timestamps/frequency/disjoint results reach the CPU.
    struct gpu_timing
    {
        IDirect3DDevice9 *device;
        com_ptr<IDirect3DQuery9> frequency,disjoint;
        std::vector<std::array<IDirect3DQuery9 *,2>> stamps;
        bool enabled=false;
        explicit gpu_timing(IDirect3DDevice9 *value):device(value){}
        ~gpu_timing(){for(auto &pair:stamps)for(auto *q:pair)if(q)q->Release();}
        void initialize(unsigned count)
        {
            if(FAILED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ,frequency.put()))||
                FAILED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT,disjoint.put())))
            {std::printf("GPU timestamp timing unavailable on this device.\n");return;}
            stamps.resize(count,{nullptr,nullptr});
            for(auto &pair:stamps)for(auto &q:pair)
                if(FAILED(device->CreateQuery(D3DQUERYTYPE_TIMESTAMP,&q)))
                {std::printf("GPU timestamp allocation unavailable.\n");return;}
            enabled=true;
        }
        void begin(unsigned index)
        {
            if(!enabled)return;
            if(index==0 && (FAILED(disjoint->Issue(D3DISSUE_BEGIN))||FAILED(frequency->Issue(D3DISSUE_END))))enabled=false;
            if(enabled&&FAILED(stamps[index][0]->Issue(D3DISSUE_END)))enabled=false;
        }
        void end(unsigned index)
        {
            if(!enabled)return;
            if(FAILED(stamps[index][1]->Issue(D3DISSUE_END)))enabled=false;
            if(enabled&&index+1==stamps.size()&&FAILED(disjoint->Issue(D3DISSUE_END)))enabled=false;
        }
        bool resolve()
        {
            if(!enabled){std::printf("GPU timestamp timing not available; no GPU-time claim.\n");return true;}
            const ULONGLONG started=GetTickCount64();
            UINT64 ticks_per_second=0;BOOL changed=TRUE;
            std::vector<std::array<UINT64,2>> values(stamps.size(),{0,0});
            bool ready=false;
            do
            {
                ready=true;
                const auto read=[&](IDirect3DQuery9 *query,void *out,DWORD size)
                {
                    const HRESULT hr=query->GetData(out,size,0);
                    if(hr==S_FALSE){ready=false;return true;}
                    return succeeded(hr,"GetData(nonblocking timestamp)");
                };
                if(!read(frequency,&ticks_per_second,sizeof(ticks_per_second))||!read(disjoint,&changed,sizeof(changed)))return false;
                for(unsigned i=0;i<stamps.size();++i)for(unsigned j=0;j<2;++j)
                    if(!read(stamps[i][j],&values[i][j],sizeof(UINT64)))return false;
                if(!ready)
                {
                    if(!succeeded(device->Present(nullptr,nullptr,nullptr,nullptr),"Present(timestamp progress)"))return false;
                    Sleep(10);
                }
            }while(!ready&&GetTickCount64()-started<5000);
            if(!ready||changed||ticks_per_second==0)
            {std::printf("GPU timestamps unavailable/disjoint in this run; no GPU-time claim.\n");return true;}
            double sum=0,maximum=0;
            for(const auto &pair:values)
            {
                if(!require_temporal(pair[1]>=pair[0],"GPU timestamp ordering invalid"))return false;
                const double elapsed=1000.0*static_cast<double>(pair[1]-pair[0])/static_cast<double>(ticks_per_second);
                sum+=elapsed;maximum=std::fmax(maximum,elapsed);
            }
            std::printf("1080p full renderer GPU timestamps after warmup: mean %.3f ms, max %.3f ms, %zu moving frames; "
                "includes motion+grayscale+history, excludes fixture scene/Present; asynchronous flags-zero queries, not real-game FPS.\n",
                sum/values.size(),maximum,values.size());
            return true;
        }
    };
}

int wmain()
{
    test_objects objects;
    wchar_t system_path[MAX_PATH]{};GetSystemDirectoryW(system_path,MAX_PATH);
    objects.module=LoadLibraryW((std::filesystem::path(system_path)/L"d3d9.dll").c_str());
    if(!objects.module)return windows_failure("LoadLibrary(system D3D9)");
    using create_fn=IDirect3D9 *(WINAPI *)(UINT);
    const auto create=std::bit_cast<create_fn>(GetProcAddress(objects.module,"Direct3DCreate9"));
    objects.window=CreateWindowExW(0,L"STATIC",L"ENR motion GPU regression",WS_OVERLAPPEDWINDOW,
        0,0,320,240,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    objects.d3d=create(D3D_SDK_VERSION);
    D3DADAPTER_IDENTIFIER9 adapter{};
    if(succeeded(objects.d3d->GetAdapterIdentifier(D3DADAPTER_DEFAULT,0,&adapter),"GetAdapterIdentifier"))
        std::printf("GPU adapter: %s (vendor 0x%04lX, device 0x%04lX)\n",adapter.Description,adapter.VendorId,adapter.DeviceId);
    D3DPRESENT_PARAMETERS pp{};pp.BackBufferWidth=320;pp.BackBufferHeight=240;pp.BackBufferFormat=D3DFMT_A8R8G8B8;
    pp.BackBufferCount=1;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.hDeviceWindow=objects.window;
    pp.Windowed=TRUE;pp.PresentationInterval=D3DPRESENT_INTERVAL_IMMEDIATE;
    if(!succeeded(objects.d3d->CreateDevice(0,D3DDEVTYPE_HAL,objects.window,
            D3DCREATE_HARDWARE_VERTEXPROCESSING|D3DCREATE_FPU_PRESERVE,&pp,&objects.device),"CreateDevice(motion fixture)"))return 1;
    enr::grayscale_d3d9 renderer;
    gpu_timing timing{objects.device};
    frame_images images{objects.device};procedural_scene scene{objects.device};motion_predicate predicate{objects.device};
    if(!images.initialize(256,192,128,96)||!scene.initialize(256,192)||!predicate.initialize())return 1;
    com_ptr<IDirect3DTexture9> app_texture;com_ptr<IDirect3DVertexBuffer9> app_vertices;
    if(!succeeded(objects.device->CreateTexture(2,2,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,app_texture.put(),nullptr),"CreateTexture(app)")||
        !succeeded(objects.device->CreateVertexBuffer(80,0,D3DFVF_XYZRHW|D3DFVF_DIFFUSE,D3DPOOL_MANAGED,app_vertices.put(),nullptr),"CreateVertexBuffer(app)"))return 1;
    std::uint64_t frame=0,generation=1;
    IDirect3DTexture9 *stable_motion=nullptr;
    bool had_motion=false;
    bool benchmark_mode=false;
    unsigned benchmark_index=0;
    std::vector<double> submission_times;
    LARGE_INTEGER timer_frequency{};QueryPerformanceFrequency(&timer_frequency);
    const auto render=[&](float dx,float dy,float z,unsigned pattern,bool depth,bool prior,
        enr::temporal_view view,float other_x=0,float other_y=0,float angle=0)
    {
        ++frame;
        if(!images.fill(0,z)||!succeeded(objects.device->BeginScene(),"BeginScene(scene)")||
            !scene.draw(images.color_surface,dx,dy,pattern,other_x,other_y,angle)||!succeeded(objects.device->EndScene(),"EndScene(scene)")||
            !set_game_state(objects.device,images.width,images.height,app_texture,app_vertices))return false;
        const float constants[20]={.12f,.23f,.34f,.45f,.56f,.67f,.78f,.89f,.91f,.82f,.73f,.64f,.55f,.46f,.37f,.28f,.19f,.21f,.32f,.43f};
        for(DWORD slot=1;slot<5;++slot)
            if(!succeeded(objects.device->SetTexture(slot,app_texture),"Bind application extra texture")||
                !succeeded(objects.device->SetSamplerState(slot,D3DSAMP_ADDRESSU,D3DTADDRESS_WRAP),"Set extra sampler")||
                !succeeded(objects.device->SetSamplerState(slot,D3DSAMP_SRGBTEXTURE,TRUE),"Set extra sRGB sampler"))return false;
        objects.device->SetPixelShader(nullptr);objects.device->SetPixelShaderConstantF(0,constants,5);
        RECT before_scissor{};D3DVIEWPORT9 before_viewport{};
        objects.device->GetScissorRect(&before_scissor);objects.device->GetViewport(&before_viewport);
        if(!succeeded(objects.device->BeginScene(),"BeginScene(renderer)"))return false;
        enr::temporal_report report;
        if(benchmark_mode)timing.begin(benchmark_index);
        LARGE_INTEGER before_render{},after_render{};QueryPerformanceCounter(&before_render);
        const auto hr=renderer.render_temporal(objects.device,images.color_surface,depth?images.depth:nullptr,
            generation,frame,view,false,false,report);
        QueryPerformanceCounter(&after_render);
        if(benchmark_mode)timing.end(benchmark_index++);
        if(benchmark_mode)submission_times.push_back(1000.0*(after_render.QuadPart-before_render.QuadPart)/timer_frequency.QuadPart);
        if(!succeeded(hr,"render_temporal(motion)")||!succeeded(report.history_result,"history result")||
            !succeeded(report.motion_result,"motion result")||
            !require_temporal(report.valid_before==prior,"incorrect motion history input validity")||
            !require_temporal(report.motion_processed==(prior&&depth),"incorrect motion processed flag")||
            !require_temporal(renderer.motion_valid()==(prior&&depth),"incorrect motion valid flag"))return false;
        if(prior&&depth)
        {
            D3DSURFACE_DESC desc{};renderer.motion_texture()->GetLevelDesc(0,&desc);
            if(!require_temporal(desc.Width==(images.width+7)/8 && desc.Height==(images.height+7)/8 &&
                    desc.Format==D3DFMT_A16B16G16R16F,"incorrect motion format/resolution"))return false;
            if(had_motion&&!require_temporal(renderer.motion_texture()==stable_motion,"steady motion texture identity changed"))return false;
            stable_motion=renderer.motion_texture();had_motion=true;
        }
        else if(!require_temporal(renderer.motion_texture()==nullptr,"stale motion texture exposed"))return false;
        RECT after_scissor{};D3DVIEWPORT9 after_viewport{};float after_constants[20]{};
        objects.device->GetScissorRect(&after_scissor);objects.device->GetViewport(&after_viewport);objects.device->GetPixelShaderConstantF(0,after_constants,5);
        if(!require_temporal(EqualRect(&before_scissor,&after_scissor)&&std::memcmp(&before_viewport,&after_viewport,sizeof(before_viewport))==0&&
            std::memcmp(constants,after_constants,sizeof(constants))==0,"motion changed viewport/scissor/constants"))return false;
        for(const auto &state:game_states)
        {DWORD actual=0;objects.device->GetRenderState(state.type,&actual);if(!require_temporal(actual==state.value,"motion changed render state"))return false;}
        for(DWORD slot=0;slot<5;++slot)
        {
            com_ptr<IDirect3DBaseTexture9> actual;DWORD sampler=0;objects.device->GetTexture(slot,actual.put());
            objects.device->GetSamplerState(slot,D3DSAMP_ADDRESSU,&sampler);
            if(!require_temporal(actual.value==app_texture.value&&sampler==D3DTADDRESS_WRAP,"motion changed texture/sampler binding"))return false;
            if(slot){objects.device->GetSamplerState(slot,D3DSAMP_SRGBTEXTURE,&sampler);if(!require_temporal(sampler==TRUE,"motion changed extra sampler sRGB"))return false;}
        }
        if(!succeeded(objects.device->EndScene(),"EndScene(renderer)"))return false;
        if(benchmark_mode)return succeeded(objects.device->Present(nullptr,nullptr,nullptr,nullptr),"Present(benchmark fixture)");
        if(
            !succeeded(objects.device->StretchRect(images.color_surface,nullptr,images.output_surface,nullptr,D3DTEXF_NONE),"Copy display to GPU predicate input")||
            !succeeded(objects.device->BeginScene(),"BeginScene(alpha predicate)")||
            !predicate.add(images.output_copy,"display alpha preserved",0,0,2,64,0,0,1,1)||
            (view==enr::temporal_view::motion && !prior && !predicate.add(images.output_copy,"unavailable motion debug is black",0,0,8,64,0,0,1,1))||
            !succeeded(objects.device->EndScene(),"EndScene(alpha predicate)")||
            !succeeded(objects.device->Present(nullptr,nullptr,nullptr,nullptr),"Present(native motion)"))return false;
        return true;
    };
    const auto compare=[&](const char *label,float x,float y,unsigned mode=0,DWORD minimum=56,
        float left=.25f,float top=.25f,float right=.75f,float bottom=.75f,float tolerance=1.01f,float angle=0)
    {
        return succeeded(objects.device->BeginScene(),"BeginScene(motion predicate)")&&
            predicate.add(renderer.motion_texture(),label,x,y,mode,minimum,left,top,right,bottom,tolerance,angle)&&
            succeeded(objects.device->EndScene(),"EndScene(motion predicate)")&&
            succeeded(objects.device->Present(nullptr,nullptr,nullptr,nullptr),"Present(motion predicate)");
    };
    const auto pair=[&](float dx,float dy,float previous_z=.2f,float current_z=.2f,unsigned pattern=0,
        enr::temporal_view view=enr::temporal_view::current_grayscale,float other_x=0,float other_y=0)
    {
        renderer.invalidate_history();
        return render(0,0,previous_z,pattern,true,false,view)&&render(dx,dy,current_z,pattern,true,true,view,other_x,other_y);
    };
    const auto compare_motion_display=[&]()
    {
        return succeeded(objects.device->BeginScene(),"BeginScene(debug color predicate)")&&
            predicate.add(images.output_copy,"motion debug renders direction colors",0,0,7,56)&&
            succeeded(objects.device->EndScene(),"EndScene(debug color predicate)")&&
            succeeded(objects.device->Present(nullptr,nullptr,nullptr,nullptr),"Present(debug color predicate)");
    };
    if(!pair(0,0)||!compare("textured static zero motion",0,0,0,60)||
        !pair(8,0)||!compare("right translation has negative X correspondence",-8,0)||
        !pair(0,8)||!compare("down translation has negative Y correspondence",0,-8)||
        !pair(6,-4,.2f,.2f,0,enr::temporal_view::motion)||!compare("diagonal translation magnitude and sign",-6,4)||
        !compare_motion_display()||
        !compare("diagonal diagnostic valid count",-6,4,4,0)||
        !compare("diagonal diagnostic within4 pixels",-6,4,0,0,.25f,.25f,.75f,.75f,4.01f)||
        !compare("diagonal diagnostic wrong sign count",-6,4,5,0)||
        !compare("out-of-bounds border correspondence invalid",0,0,1,64,0,0,.025f,1)||
        !pair(-8,0)||!compare("left translation has positive X correspondence",8,0)||
        !pair(8,0,.2f,.2f,2,enr::temporal_view::motion,0,-8)||
        !compare("independent left region moves right",-8,0,0,48,.2f,.3f,.35f,.7f)||
        !compare("independent right region moves up",0,8,0,48,.65f,.3f,.8f,.7f)||
        !pair(0,0,.2f,.8f)||!compare("incompatible scene depth rejects match",0,0,1,64)||
        !pair(0,0,.2f,.2f,1)||!compare("flat dark area rejects ambiguous motion",0,0,1,64)||
        !pair(0,0,1,1)||!compare("clear scene depth rejects match",0,0,1,64))return 1;
    renderer.invalidate_history();
    constexpr float rotation_angle=.025f;
    if(!render(0,0,.2f,0,true,false,enr::temporal_view::motion)||
        !render(0,0,.2f,0,true,true,enr::temporal_view::motion,0,0,rotation_angle)||
        !compare("bounded camera-like rotation correspondence",0,0,6,48,.2f,.2f,.8f,.8f,2.01f,rotation_angle))return 1;
    ++generation;
    if(!render(0,0,.2f,0,true,false,enr::temporal_view::motion)||
        !render(0,0,.2f,0,true,true,enr::temporal_view::motion)||
        !render(0,0,.2f,0,false,false,enr::temporal_view::motion))return 1;
    if(!render(0,0,.2f,0,true,false,enr::temporal_view::motion))return 1;
    frame+=2;
    if(!render(0,0,.2f,0,true,false,enr::temporal_view::motion))return 1;
    renderer.reset();had_motion=false;
    if(!render(0,0,.2f,0,true,false,enr::temporal_view::motion)||
        !render(8,0,.2f,0,true,true,enr::temporal_view::motion)||!compare("motion resumes after renderer reset",-8,0))return 1;
    had_motion=false;
    if(!images.initialize(1920,1080,1920,1080)||!scene.initialize(1920,1080)||
        !pair(8,0,.2f,.2f,0,enr::temporal_view::motion)||!compare("1920x1080 motion sign and magnitude",-8,0))return 1;
    benchmark_mode=true;
    timing.initialize(20);
    for(unsigned iteration=0;iteration<20;++iteration)
        if(!render(9.0f+iteration,0,.2f,0,true,true,enr::temporal_view::current_grayscale))return 1;
    double average=0,maximum=0;
    for(const double value:submission_times){average+=value;maximum=std::fmax(maximum,value);}
    std::printf("1080p renderer CPU-call wall time after warmup: mean %.3f ms, max %.3f ms, %zu calls; includes driver submission/backpressure, not GPU timestamps or real-game FPS.\n",
        average/submission_times.size(),maximum,submission_times.size());
    if(!predicate.resolve())return 1;
    if(!timing.resolve())return 1;
    std::printf("PASS: GPU-only block motion static/horizontal/vertical/diagonal/independent regions, rejection, first/reset/resize/gap/depth invalidation, "
        "persistent texture identity, 1920x1080, state and alpha preservation. No CPU pixels or forced GPU flush.\n");
    return 0;
}
