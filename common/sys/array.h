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

#include "platform.h"

namespace embree
{
  template<typename T, size_t N>
    class array_t
    {
    public:

      /********************** Iterators  ****************************/

      __forceinline T* begin() const { return items; };
      __forceinline T* end  () const { return items+N; };


      /********************** Capacity ****************************/

      __forceinline bool   empty    () const { return N == 0; }
      __forceinline size_t size     () const { return N; }
            

      /******************** Element access **************************/

      __forceinline       T& operator[](size_t i)       { assert(i < N); return items[i]; }
      __forceinline const T& operator[](size_t i) const { assert(i < N); return items[i]; }

      __forceinline       T& at(size_t i)       { assert(i < N); return items[i]; }
      __forceinline const T& at(size_t i) const { assert(i < N); return items[i]; }

      __forceinline T& front() const { assert(N > 0); return items[0]; };
      __forceinline T& back () const { assert(N > 0); return items[N-1]; };

      __forceinline       T* data()       { return items; };
      __forceinline const T* data() const { return items; };

    private:
      T items[N];
    };
}
