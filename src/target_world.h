#pragma once
#include "ui_objective_world.h"
#include <cmath>
#include <initializer_list>
namespace TargetWorld {
struct Group {
    float roots[8][2]{}; unsigned count{}; bool valid=true;
    float capture[2]{},captureScale=1;
    void Add(float x,float y,float u,float v) {
        if(!count){capture[0]=x;capture[1]=y;}
        if(count<8) {
            const float expected=(count%2)?1.f:38.f;
            valid=valid && std::isfinite(x) && std::isfinite(y) &&
                std::fabs(u*240-expected)<.75f && std::fabs(v*176-117)<.75f;
            roots[count][0]=x;roots[count][1]=y;
        }
        ++count;
    }
    bool Centre(float& x,float& y) const {
        if(!valid || count<8)return false;
        x=y=0;
        for(unsigned i=0;i<4;++i){x+=roots[i][0]*.25f;y+=roots[i][1]*.25f;}
        // Four opposite pairs independently identify the same unsnapped centre.
        for(unsigned i : {0u,1u,4u,5u})
            if(std::fabs((roots[i][0]+roots[i+2][0])*.5f-x)>.02f ||
               std::fabs((roots[i][1]+roots[i+2][1])*.5f-y)>.02f)return false;
        const float diameter=std::hypot(roots[0][0]-roots[2][0],roots[0][1]-roots[2][1]);
        return std::isfinite(diameter) && diameter>4 && diameter<4096;
    }
};
}
