// ======================================================================== //
// Copyright 2009-2015 Intel Corporation                                    //
//                                                                          //
// Licensed under the Apache License, Version 2.0 (the "License");          //
// you may not use this file except in compliance with the License.         //
// You may obtain a copy of the License at                                  //
//                                                                          //
//     http://www.apache.org/licenses/LICENSE-2.0                           //
//                                                                          //
// Unless required by applicable law or agreed to in writing, software      //
// distributed under the License is distributed on an "AS IS" BASIS,        //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. //
// See the License for the specific language governing permissions and      //
// limitations under the License.                                           //
// ======================================================================== //

#pragma once

#include "bezier1v.h"
#include "bezier_intersector.h"

namespace embree
{
  namespace isa
  {
    /*! Intersector for a single ray with a bezier curve. */
    struct Bezier1vIntersector1
    {
      typedef Bezier1v Primitive;
      typedef Bezier1Intersector1::Precalculations Precalculations;
      
      static __forceinline void intersect(Precalculations& pre, Ray& ray, const Primitive& curve, Scene* scene) {
        Bezier1Intersector1::intersect(ray,pre,curve.p0,curve.p1,curve.p2,curve.p3,curve.geomID(),curve.primID(),scene);
      }
      
      static __forceinline bool occluded(Precalculations& pre, Ray& ray, const Primitive& curve, Scene* scene) {
        return Bezier1Intersector1::occluded(ray,pre,curve.p0,curve.p1,curve.p2,curve.p3,curve.geomID(),curve.primID(),scene);
      }
    };
  }
}
