#pragma once
// Synthetic layouts are invented original-reach graphs, not compiled collision assets.
namespace overlay_four {

struct NativeData4 {
    fixture_t input;
    std::unique_ptr<FixtureBase> walk,fly;
    std::vector<Door> wd,fd;
    std::vector<U> rawStart,rawIndex;
    NativeData4() {
        const char* aas="plans/map-graph/gates-2b/maps/s2b0d400/cE0N0.aas48";
        const char* map="plans/map-graph/gates-2b/maps/s2b0d400/cE0N0.map";
        Require(ReadFixture(aas,input),"overlay4: native fixture unreadable");
        input.low[0]=input.low[1]=-4096; input.low[2]=-576;
        input.high[0]=input.high[1]=4096; input.high[2]=3136;
        walk=std::make_unique<FixtureBase>(MakeBase(input,4194306,false));
        fly=std::make_unique<FixtureBase>(MakeBase(input,4196354,true));
        SharedSeams(*walk,*fly);
        const auto geometry=ReadCensus(aas,map);
        wd=CensusDoors(geometry,input,walk->graph,"walk");
        fd=CensusDoors(geometry,input,fly->graph,"fly-DIAGNOSTIC");
        SetCensus(*walk,wd); SetCensus(*fly,fd);
        rawStart.assign(input.flags.size()+1,0);
        for(const auto&r:input.reaches)rawStart[r.from+1]++;
        std::partial_sum(rawStart.begin(),rawStart.end(),rawStart.begin());
        rawIndex.assign(input.reaches.size(),NONE);
        for(U i=0;i<input.reaches.size();i++){
            const auto&r=input.reaches[i];
            Require(rawStart[r.from]+U(r.ordinal)<rawStart[r.from+1],"overlay4: raw ordinal range");
            rawIndex[rawStart[r.from]+r.ordinal]=i;
        }
        for(const auto* b:{walk.get(),fly.get()}) {
            std::vector<double> spacing;
            for(const auto&e:b->graph.edges) {
                const Point4 a{b->centres[3*e.from],b->centres[3*e.from+1],b->centres[3*e.from+2]};
                const Point4 c{b->centres[3*e.to],b->centres[3*e.to+1],b->centres[3*e.to+2]};
                if(Distance4(a,c)>0)spacing.push_back(Distance4(a,c));
            }
            printf("M nativeCentreSpacing4 profile=%s edges=%zu median=%.6f p99=%.6f (centre-spacing-not-area-diameter)\n",b==walk.get()?"walk":"fly-DIAGNOSTIC",spacing.size(),Quantile(spacing,50),Quantile(spacing,99));
        }
    }
};
static NativeData4& NativeInputs4(){static NativeData4 data;return data;}
static Point4 ShiftPoint4(Point4 p,U tile,U side){p.x+=8192.0*(tile%side);p.y+=8192.0*(tile/side);return p;}

static Fixture4 MakeNative4(bool flying,U side,bool planted=false) {
    Require(side==1||side==8,"overlay4: native side must be inherited fixture");
    Require(!planted||side==8,"overlay4: bridges require stitched fixture");
    auto& native=NativeInputs4(); Fixture4 f;
    f.fly=flying;f.side=side;f.name=planted?"planted8x8":side==1?"d400":"stitch8x8";
    f.base=std::make_unique<FixtureBase>(flying?*native.fly:*native.walk);
    auto& b=*f.base; const U originalN=U(native.input.flags.size());
    if(planted) {
        f.bridge=PlantBridge(b);f.control=424;
        U anchor=NONE;
        for(const auto&e:b.graph.edges)if(U(e.to)==f.bridge&&U(e.from)<originalN){anchor=U(e.from);break;}
        Require(anchor!=NONE,"overlay4: missing bridge anchor");
        // Existing bridge w remains frozen; geometric witness receives explicit positive synthetic legs.
        for(U v=f.bridge;v<U(b.graph.n);v++) {
            b.centres[3*v]=b.centres[3*anchor]+float(v==f.bridge?128:256+16*(v-f.bridge));
            b.centres[3*v+1]=b.centres[3*anchor+1];b.centres[3*v+2]=b.centres[3*anchor+2];
        }
        Recut(b);
        printf("A planted geometry4 positive straight legs: bridge x=anchor+128; leaf_i x=anchor+256+16*i; inherited scalar w unchanged; synthetic geometry only\n");
    }
    const U N=U(b.graph.n), copies=side*side;
    f.g=Replicate(b,native.input,side,flying);
    f.census=DoorCopies(flying?native.fd:native.wd,N,copies);
    f.centres.resize(f.g.n);f.water.assign(f.g.n,0);
    for(U t=0;t<copies;t++)for(U a=0;a<N;a++) {
        f.centres[t*N+a]=ShiftPoint4({b.centres[3*a],b.centres[3*a+1],b.centres[3*a+2]},t,side);
        if(a<originalN)f.water[t*N+a]=(native.input.contents[a]&2)!=0;
    }
    if(planted) {
        for(U t=0;t<copies;t++) {
            const U x=t%side,y=t/side;
            const bool active=x>=2&&x<=5&&y>=2&&y<=5;
            f.g.mutableArea[t*N+f.bridge]=active;
            f.g.mutableArea[t*N+424]=(t==0);
            if(active) {
                Door d;d.name="bridge_tile"+std::to_string(t);d.areas={t*N+f.bridge};f.bridges.push_back(d);
                U anchor=NONE;for(U i=f.g.in[t*N+f.bridge];i<f.g.in[t*N+f.bridge+1];i++) {
                    U v=f.g.edges[f.g.rev[i]].from;if(v%N<originalN){anchor=v;break;}
                }
                Require(anchor!=NONE,"overlay4: replicated bridge anchor");
                f.plants.emplace_back(anchor,t*N+f.bridge+1);
            }
        }
        Door control;control.name="area424_control_tile0";control.areas={424};f.bridges.push_back(control);
    }
    std::vector<std::array<Point4,4>> anchor(N);
    std::vector<std::array<uint8_t,4>> have(N);
    for(U s=0;s<4;s++)for(const auto&a:b.border[s]){anchor[a.area][s]={a.p[0],a.p[1],a.p[2]};have[a.area][s]=1;}
    f.geo.resize(f.g.edges.size());W raw=0,seam=0,added=0,fallback=0;
    for(U i=0;i<f.g.edges.size();i++) {
        const auto&e=f.g.edges[i];const U a=e.from%N,c=e.to%N,t=e.from/N,u=e.to/N;
        Geo4 geometry;
        if(t==u&&a<originalN&&e.ordinal<native.rawStart[a+1]-native.rawStart[a]) {
            const U ri=native.rawIndex[native.rawStart[a]+e.ordinal];
            Require(ri!=NONE,"overlay4: native geometry ordinal missing");const auto&r=native.input.reaches[ri];
            Require(U(r.to)==c,"overlay4: native geometry endpoint mismatch");
            geometry.start=ShiftPoint4({r.start[0],r.start[1],r.start[2]},t,side);
            geometry.end=ShiftPoint4({r.end[0],r.end[1],r.end[2]},u,side);raw++;
        } else if(t!=u) {
            U ax=(t/side==u/side)?0:1;bool positive=(ax?t/side:t%side)<(ax?u/side:u%side);
            U sa=2*ax+U(positive),sc=2*ax+U(!positive);
            auto point=[&](U v,U s) {
                if(have[v][s])return anchor[v][s];
                fallback++;Point4 p{b.centres[3*v],b.centres[3*v+1],b.centres[3*v+2]};
                if(s/2==0)p.x=(s%2)?4056:-4056;else p.y=(s%2)?4056:-4056;return p;
            };
            geometry.start=ShiftPoint4(point(a,sa),t,side);geometry.end=ShiftPoint4(point(c,sc),u,side);seam++;
        } else {geometry={f.centres[e.from],f.centres[e.to]};added++;}
        Require(std::isfinite(Distance4(geometry.start,geometry.end)),"overlay4: nonfinite fixture geometry");f.geo[i]=geometry;
    }
    printf("M fixture4 kind=%s profile=%s areas=%u reaches=%zu nativeGeometry=%llu seamGeometry=%llu plantedGeometry=%llu seamAnchorFallback=%llu censusDoors=%zu bridgeAndControlDoors=%zu\n",f.name.c_str(),flying?"fly-DIAGNOSTIC":"walk",f.g.n,f.g.edges.size(),(unsigned long long)raw,(unsigned long long)seam,(unsigned long long)added,(unsigned long long)fallback,f.census.size(),f.bridges.size());
    return f;
}

struct District4 {int x0,y0,x1,y1,type;};
static Fixture4 MakeSynthetic4(const std::string&kind,bool flying) {
    Require(kind=="open"||kind=="maze"||kind=="mixed","overlay4: unknown synthetic kind");
    Fixture4 f;f.name=kind;f.fly=flying;f.synthetic=true;
    // Assumed cell pitches reflect a terrain-scale discretization; the native spacing census is printed separately.
    const U width=flying?256:512,layers=flying?4:1;
    const double pitch=65536.0/width;const int scale=int(width/128);
    const U n=width*width*layers;
    auto id=[&](int x,int y,int z=0){return U(z)*width*width+U(y)*width+U(x);};
    std::vector<District4> districts;
    if(kind!="open") {
        districts.push_back({16*scale,16*scale,32*scale,32*scale,1}); // serpentine 8192-u district
        districts.push_back({48*scale,16*scale,64*scale,32*scale,2}); // comb
        districts.push_back({80*scale,16*scale,96*scale,32*scale,3}); // ring
    }
    if(kind=="mixed")for(int k=0;k<4;k++)districts.push_back({(16+24*k)*scale,88*scale,(24+24*k)*scale,96*scale,4});
    std::vector<int> district(width*width,-1);
    std::vector<uint8_t> valid(width*width,1);
    for(U di=0;di<districts.size();di++) {
        const auto&d=districts[di];
        for(int y=d.y0;y<d.y1;y++)for(int x=d.x0;x<d.x1;x++) {
            district[U(y)*width+x]=int(di);
            if(d.type==2)valid[U(y)*width+x]=(y==d.y0)||((x-d.x0)%2==0);
            if(d.type==3)valid[U(y)*width+x]=(x==d.x0||x==d.x1-1||y==d.y0||y==d.y1-1);
        }
    }
    auto gate=[&](const District4&d){return std::pair<int,int>{d.type==4?(d.x0+d.x1)/2:d.x0,d.y0};};
    auto permitted=[&](int x,int y,int xx,int yy) {
        const int da=district[U(y)*width+x],db=district[U(yy)*width+xx];
        const int dx=std::abs(xx-x),dy=std::abs(yy-y);
        if(da!=db) {
            if(dx+dy!=1)return false;
            if(da>=0){auto p=gate(districts[da]);if(x!=p.first||y!=p.second)return false;}
            if(db>=0){auto p=gate(districts[db]);if(xx!=p.first||yy!=p.second)return false;}
        } else if(da>=0) {
            const auto&d=districts[da];
            if(d.type<4&&dx+dy!=1)return false;
            if(d.type==1&&dy) {
                const int row=std::min(y,yy)-d.y0;
                const int crossing=(row%2==0)?d.x1-1:d.x0;
                if(x!=crossing||xx!=crossing)return false;
            }
        }
        // A wall 16384 u long spans every flying layer; its only middle opening is a declared door area.
        if(kind!="open") {
            const int wall=64*scale,lo=48*scale,hi=80*scale,doorY=64*scale;
            if((x<wall)!=(xx<wall)) {
                const int minY=std::min(y,yy),maxY=std::max(y,yy);
                if(maxY>=lo&&minY<hi&&!(y==doorY&&yy==doorY))return false;
            }
        }
        return true;
    };
    f.centres.resize(n);f.water.assign(n,0);
    f.base=std::make_unique<FixtureBase>(int(n));auto&b=*f.base;
    b.centres.resize(size_t(n)*3);b.degree.assign(n,0);
    for(U z=0;z<layers;z++)for(U y=0;y<width;y++)for(U x=0;x<width;x++) {
        const U v=id(int(x),int(y),int(z));
        const double px=(x+0.5)*pitch,py=(y+0.5)*pitch;
        const double height=32*std::sin(px/4096.0)+24*std::cos(py/4096.0)+((U(px)/8192)%2==0?96:0);
        f.centres[v]={px,py,height+(flying?512+512*z:0)};
        b.centres[3*v]=float(px);b.centres[3*v+1]=float(py);b.centres[3*v+2]=float(f.centres[v].z);
        b.graph.enabled[v]=valid[y*width+x];
    }
    auto declareDoor=[&](int x,int y,const std::string&name) {
        Door d;d.name=name;
        for(U z=0;z<layers;z++){U v=id(x,y,int(z));Require(b.graph.enabled[v],"overlay4: synthetic door in void");b.graph.mutableArea[v]=1;d.areas.push_back(v);}
        f.census.push_back(std::move(d));
    };
    if(kind=="open") {
        for(U k=0;k<4;k++)declareDoor(int((24+24*k)*scale),int(64*scale),"open_column_"+std::to_string(k));
        f.plants.emplace_back(id(2,2),id(int(width)-3,int(width)-3,int(layers)-1));
    } else {
        for(U k=0;k<districts.size();k++){auto p=gate(districts[k]);declareDoor(p.first,p.second,"district_entrance_"+std::to_string(k));}
        const auto&snake=districts[0];f.plants.emplace_back(id(snake.x0+1,snake.y0),id(snake.x0+1,snake.y0+2));
        const auto&comb=districts[1];f.plants.emplace_back(id(comb.x0,comb.y1-1),id(comb.x0+2,comb.y1-1));
        const auto&ring=districts[2];const int mid=(ring.x0+ring.x1)/2;
        declareDoor(mid,ring.y0,"ring_shortcut");f.plants.emplace_back(id(mid-1,ring.y0),id(mid+1,ring.y0));
        declareDoor(64*scale,64*scale,"long_wall_shortcut");f.plants.emplace_back(id(64*scale-1,64*scale),id(64*scale+1,64*scale));
        for(U k=3;k<districts.size();k++){const auto&d=districts[k];f.plants.emplace_back(id(d.x0-1,d.y0),id(d.x1-2,d.y1-2,int(layers)-1));}
    }
    W drops=0,vertical=0;
    for(U z=0;z<layers;z++)for(U y=0;y<width;y++)for(U x=0;x<width;x++) {
        U from=id(int(x),int(y),int(z));if(!b.graph.enabled[from])continue;
        auto add=[&](U to){double length=Distance4(f.centres[from],f.centres[to]);Require(length>0,"overlay4: synthetic zero edge");const U w=1+U(std::ceil(length*100.0/300.0));b.graph.Add(int(from),int(to),w,int(b.degree[from]++));};
        for(int dy=-1;dy<=1;dy++)for(int dx=-1;dx<=1;dx++)if(dx||dy) {
            int xx=int(x)+dx,yy=int(y)+dy;if(xx<0||yy<0||xx>=int(width)||yy>=int(width))continue;
            U to=id(xx,yy,int(z));if(!b.graph.enabled[to]||!permitted(int(x),int(y),xx,yy))continue;
            // Corner edges are allowed only when both adjacent cardinal transitions are legal and nonvoid.
            if(dx&&dy) {
                if(!valid[y*width+U(xx)]||!valid[U(yy)*width+x]||
                   !permitted(int(x),int(y),xx,int(y))||!permitted(int(x),int(y),int(x),yy)||
                   !permitted(xx,int(y),xx,yy)||!permitted(int(x),yy,xx,yy))continue;
            }
            const bool wallOpening=kind!="open"&&int(y)==64*scale&&yy==64*scale&&((int(x)<64*scale)!=(xx<64*scale));
            if(!flying&&!wallOpening&&f.centres[to].z-f.centres[from].z>64&&U(f.centres[from].y)%8192<512){drops++;continue;}
            add(to);
        }
        if(flying){if(z)add(from-width*width);if(z+1<layers)add(from+width*width);vertical+=(z>0)+(z+1<layers);}
    }
    b.graph.Finish();
    std::vector<int> labels(n);
    for(U v=0;v<n;v++){const auto&p=f.centres[v];labels[v]=int(p.y/2048.0)*32+int(p.x/2048.0);}
    Require(Partition(b.graph,2048,32768,b.cut,caps_t(),&labels),"overlay4: synthetic partition refused");
    f.g=SmallGraph(b.graph);f.geo.resize(f.g.edges.size());
    for(U ei=0;ei<f.g.edges.size();ei++){const auto&e=f.g.edges[ei];f.geo[ei]={f.centres[e.from],f.centres[e.to]};}
    U live=0;for(uint8_t v:f.g.enabled)live+=v!=0;
    printf("A synthetic4 kind=%s profile=%s extent=65536x65536 pitch=%.0f layers=%u verticalPitch=512; rolling z=32*sin(x/4096)+24*cos(y/4096)+alternating96; sparse upward cliff reaches omitted in512u bands; centres-to-centres reaches; w=1+ceil(length*100/300); cells256fly/128walk assumed not calibrated or compiled AAS\n",kind.c_str(),flying?"fly":"walk",pitch,layers);
    if(kind!="open")printf("A mazeGeometry4 serpentDistrict=8192x8192; comb and ring districts=8192x8192; wallLength=16384, shortcut at midpoint, near-pair-horizontal<=1024, wall ends>4096 away; district walls span all flying layers; mixed buildings4096x4096; entrance closures atomic across layers\n");
    printf("M fixture4 kind=%s profile=%s areas=%u active=%u reaches=%zu cells=%zu removedUpwardReaches=%llu verticalReaches=%llu doors=%zu plantedPairs=%zu\n",kind.c_str(),flying?"fly":"walk",f.g.n,live,f.g.edges.size(),b.cut.members.size(),(unsigned long long)drops,(unsigned long long)vertical,f.census.size(),f.plants.size());
    return f;
}
}
