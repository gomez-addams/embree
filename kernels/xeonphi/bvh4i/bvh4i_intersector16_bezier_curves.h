// ======================================================================== //
// Copyright 2009-2013 Intel Corporation                                    //
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

#include "bvh4i.h"
#include "bvh4i_traversal.h"
#include "common/ray16.h" 
#include "common/accelset.h"

namespace embree
{
  namespace isa
  {
    /*! BVH4i Traverser. Packet traversal implementation for a Quad BVH. */
    class BVH4iIntersector16BezierCurves
    {
      /* shortcuts for frequently used types */
      //typedef typename BezierCurvesIntersector16::Primitive Primitive;
      typedef typename BVH4i::NodeRef NodeRef;
      typedef typename BVH4i::Node Node;
      
    public:
      static void intersect(mic_i* valid, BVH4i* bvh, Ray16& ray);
      static void occluded (mic_i* valid, BVH4i* bvh, Ray16& ray);
    };

    class BVH4iIntersector1BezierCurves
    {
      /* shortcuts for frequently used types */
      //typedef typename BezierCurvesIntersector1::Primitive Primitive;
      typedef typename BVH4i::NodeRef NodeRef;
      typedef typename BVH4i::Node Node;
      
    public:
      static void intersect(BVH4i* bvh, Ray& ray);
      static void occluded(BVH4i* bvh, Ray& ray);

    };

  }
}
