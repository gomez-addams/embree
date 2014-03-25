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

#include "bvh4i_intersector16_hybrid.h"
#include "geometry/triangle1.h"
#include "geometry/filter.h"

#define EXTENDED_PREFETCHING
#define SWITCH_ON_DOWN_TRAVERSAL 1

namespace embree
{
  namespace isa
  {
    static unsigned int BVH4I_LEAF_MASK = BVH4i::leaf_mask; // needed due to compiler efficiency bug
    static unsigned int M_LANE_7777 = 0x7777; // needed due to compiler efficiency bug

    static __aligned(64) int zlc4[4] = {0xffffffff,0xffffffff,0xffffffff,0};

    void BVH4iIntersector16Hybrid::intersect(mic_i* valid_i, BVH4i* bvh, Ray16& ray16)
    {
      /* near and node stack */
      __aligned(64) mic_f   stack_dist[3*BVH4i::maxDepth+1];
      __aligned(64) NodeRef stack_node[3*BVH4i::maxDepth+1];
      __aligned(64) NodeRef stack_node_single[3*BVH4i::maxDepth+1]; 

      /* load ray */
      const mic_m valid0     = *(mic_i*)valid_i != mic_i(0);
      const mic3f rdir16     = rcp_safe(ray16.dir);
      const mic3f org_rdir16 = ray16.org * rdir16;
      mic_f ray_tnear        = select(valid0,ray16.tnear,pos_inf);
      mic_f ray_tfar         = select(valid0,ray16.tfar ,neg_inf);
      const mic_f inf        = mic_f(pos_inf);
      
      /* allocate stack and push root node */
      stack_node[0] = BVH4i::invalidNode;
      stack_dist[0] = inf;
      stack_node[1] = bvh->root;
      stack_dist[1] = ray_tnear; 
      NodeRef* __restrict__ sptr_node = stack_node + 2;
      mic_f*   __restrict__ sptr_dist = stack_dist + 2;
      
      const Node      * __restrict__ nodes = (Node     *)bvh->nodePtr();
      const Triangle1 * __restrict__ accel = (Triangle1*)bvh->triPtr();

      while (1) pop:
      {
        /* pop next node from stack */
        NodeRef curNode = *(sptr_node-1);
        mic_f curDist   = *(sptr_dist-1);
        sptr_node--;
        sptr_dist--;
	const mic_m m_stackDist = ray_tfar > curDist;

	/* stack emppty ? */
        if (unlikely(curNode == BVH4i::invalidNode))  break;
        
        /* cull node if behind closest hit point */
        if (unlikely(none(m_stackDist))) continue;
        
	///////////////////////////////////////////////////////////////////////////////////////////////////////////////
	///////////////////////////////////////////////////////////////////////////////////////////////////////////////
	///////////////////////////////////////////////////////////////////////////////////////////////////////////////

	/* switch to single ray mode */
        if (unlikely(countbits(m_stackDist) <= BVH4i::hybridSIMDUtilSwitchThreshold)) 
	  {
	    float   *__restrict__ stack_dist_single = (float*)sptr_dist;
	    store16f(stack_dist_single,inf);

	    /* traverse single ray */	  	  
	    long rayIndex = -1;
	    while((rayIndex = bitscan64(rayIndex,m_stackDist)) != BITSCAN_NO_BIT_SET_64) 
	      {	    
		stack_node_single[0] = BVH4i::invalidNode;
		stack_node_single[1] = curNode;
		size_t sindex = 2;

		const mic_f org_xyz      = loadAOS4to16f(rayIndex,ray16.org.x,ray16.org.y,ray16.org.z);
		const mic_f dir_xyz      = loadAOS4to16f(rayIndex,ray16.dir.x,ray16.dir.y,ray16.dir.z);
		const mic_f rdir_xyz     = loadAOS4to16f(rayIndex,rdir16.x,rdir16.y,rdir16.z);
		const mic_f org_rdir_xyz = org_xyz * rdir_xyz;
		const mic_f min_dist_xyz = broadcast1to16f(&ray16.tnear[rayIndex]);
		mic_f       max_dist_xyz = broadcast1to16f(&ray16.tfar[rayIndex]);

		const unsigned int leaf_mask = BVH4I_LEAF_MASK;

		while (1) 
		  {
		    NodeRef curNode = stack_node_single[sindex-1];
		    sindex--;
            
		    traverse_single_intersect(curNode,
					      sindex,
					      rdir_xyz,
					      org_rdir_xyz,
					      min_dist_xyz,
					      max_dist_xyz,
					      stack_node_single,
					      stack_dist_single,
					      nodes,
					      leaf_mask);
	    

		    /* return if stack is empty */
		    if (unlikely(curNode == BVH4i::invalidNode)) break;


		    /* intersect one ray against four triangles */
		    const mic_f zero = mic_f::zero();

		    const Triangle1* tptr  = (Triangle1*) curNode.leaf(accel);
		    prefetch<PFHINT_L1>(tptr + 3);
		    prefetch<PFHINT_L1>(tptr + 2);
		    prefetch<PFHINT_L1>(tptr + 1);
		    prefetch<PFHINT_L1>(tptr + 0); 

		    const mic_i and_mask = broadcast4to16i(zlc4);
	      
		    const mic_f v0 = gather_4f_zlc(and_mask,
						   (float*)&tptr[0].v0,
						   (float*)&tptr[1].v0,
						   (float*)&tptr[2].v0,
						   (float*)&tptr[3].v0);
	      
		    const mic_f v1 = gather_4f_zlc(and_mask,
						   (float*)&tptr[0].v1,
						   (float*)&tptr[1].v1,
						   (float*)&tptr[2].v1,
						   (float*)&tptr[3].v1);
	      
		    const mic_f v2 = gather_4f_zlc(and_mask,
						   (float*)&tptr[0].v2,
						   (float*)&tptr[1].v2,
						   (float*)&tptr[2].v2,
						   (float*)&tptr[3].v2);

		    const mic_f e1 = v1 - v0;
		    const mic_f e2 = v0 - v2;	     
		    const mic_f normal = lcross_zxy(e1,e2);
		    const mic_f org = v0 - org_xyz;
		    const mic_f odzxy = msubr231(org * swizzle(dir_xyz,_MM_SWIZ_REG_DACB), dir_xyz, swizzle(org,_MM_SWIZ_REG_DACB));
		    const mic_f den = ldot3_zxy(dir_xyz,normal);	      
		    const mic_f rcp_den = rcp(den);
		    const mic_f uu = ldot3_zxy(e2,odzxy); 
		    const mic_f vv = ldot3_zxy(e1,odzxy); 
		    const mic_f u = uu * rcp_den;
		    const mic_f v = vv * rcp_den;

#if defined(__BACKFACE_CULLING__)
		    const mic_m m_init = (mic_m)0x1111 & (den > zero);
#else
		    const mic_m m_init = 0x1111;
#endif

		    const mic_m valid_u = ge(m_init,u,zero);
		    const mic_m valid_v = ge(valid_u,v,zero);
		    const mic_m m_aperture = le(valid_v,u+v,mic_f::one()); 

		    const mic_f nom = ldot3_zxy(org,normal);
		    if (unlikely(none(m_aperture))) continue;
		    const mic_f t = rcp_den*nom;

		    mic_m m_final  = lt(lt(m_aperture,min_dist_xyz,t),t,max_dist_xyz);

#if defined(__USE_RAY_MASK__)
		    const mic_i rayMask(ray16.mask[rayIndex]);
		    const mic_i triMask = swDDDD(gather16i_4i_align(&tptr[0].v2,&tptr[1].v2,&tptr[2].v2,&tptr[3].v2));
		    const mic_m m_ray_mask = (rayMask & triMask) != mic_i::zero();
		    m_final &= m_ray_mask;	      
#endif

                    /* did the ray hot one of the four triangles? */
		    if (unlikely(any(m_final)))
		      {
		  /* intersection filter test */
#if defined(__INTERSECTION_FILTER__) 

			mic_f org_max_dist_xyz = max_dist_xyz;

			/* did the ray hit one of the four triangles? */
			while (any(m_final)) 
			  {
			    max_dist_xyz  = select(m_final,t,org_max_dist_xyz);
			    const mic_f min_dist = vreduce_min(max_dist_xyz);
			    const mic_m m_dist = eq(min_dist,max_dist_xyz);
			    const size_t vecIndex = bitscan(toInt(m_dist));
			    const size_t triIndex = vecIndex >> 2;
			    const Triangle1  *__restrict__ tri_ptr = tptr + triIndex;
			    const mic_m m_tri = m_dist^(m_dist & (mic_m)((unsigned int)m_dist - 1));
			    const mic_f gnormalx = mic_f(tri_ptr->Ng.x);
			    const mic_f gnormaly = mic_f(tri_ptr->Ng.y);
			    const mic_f gnormalz = mic_f(tri_ptr->Ng.z);
			    const int geomID = tri_ptr->geomID();
			    const int primID = tri_ptr->primID();
                
			    Geometry* geom = ((Scene*)bvh->geometry)->get(geomID);
			    if (likely(!geom->hasIntersectionFilter16())) 
			      {

				compactustore16f_low(m_tri,&ray16.tfar[rayIndex],min_dist);
				compactustore16f_low(m_tri,&ray16.u[rayIndex],u); 
				compactustore16f_low(m_tri,&ray16.v[rayIndex],v); 
				compactustore16f_low(m_tri,&ray16.Ng.x[rayIndex],gnormalx); 
				compactustore16f_low(m_tri,&ray16.Ng.y[rayIndex],gnormaly); 
				compactustore16f_low(m_tri,&ray16.Ng.z[rayIndex],gnormalz); 
				ray16.geomID[rayIndex] = geomID;
				ray16.primID[rayIndex] = primID;
				max_dist_xyz = min_dist;
				break;
			      }
                
			    if (runIntersectionFilter16(geom,ray16,rayIndex,u,v,min_dist,gnormalx,gnormaly,gnormalz,m_tri,geomID,primID)) {
			      max_dist_xyz = min_dist;
			      break;
			    }
			    m_final ^= m_tri;
			  }
			max_dist_xyz = ray16.tfar[rayIndex];
#else

			prefetch<PFHINT_L1EX>(&ray16.tfar);  
			prefetch<PFHINT_L1EX>(&ray16.u);
			prefetch<PFHINT_L1EX>(&ray16.v);
			prefetch<PFHINT_L1EX>(&ray16.Ng.x); 
			prefetch<PFHINT_L1EX>(&ray16.Ng.y); 
			prefetch<PFHINT_L1EX>(&ray16.Ng.z); 
			prefetch<PFHINT_L1EX>(&ray16.geomID);
			prefetch<PFHINT_L1EX>(&ray16.primID);

                        max_dist_xyz  = select(m_final,t,max_dist_xyz);
			const mic_f min_dist = vreduce_min(max_dist_xyz);
			const mic_m m_dist = eq(min_dist,max_dist_xyz);

			const size_t vecIndex = bitscan(toInt(m_dist));
			const size_t triIndex = vecIndex >> 2;

			const Triangle1  *__restrict__ tri_ptr = tptr + triIndex;

			const mic_m m_tri = m_dist^(m_dist & (mic_m)((unsigned int)m_dist - 1));

			const mic_f gnormalx = mic_f(tri_ptr->Ng.x);
			const mic_f gnormaly = mic_f(tri_ptr->Ng.y);
			const mic_f gnormalz = mic_f(tri_ptr->Ng.z);
		  
			max_dist_xyz = min_dist;

			compactustore16f_low(m_tri,&ray16.tfar[rayIndex],min_dist);
			compactustore16f_low(m_tri,&ray16.u[rayIndex],u); 
			compactustore16f_low(m_tri,&ray16.v[rayIndex],v); 
			compactustore16f_low(m_tri,&ray16.Ng.x[rayIndex],gnormalx); 
			compactustore16f_low(m_tri,&ray16.Ng.y[rayIndex],gnormaly); 
			compactustore16f_low(m_tri,&ray16.Ng.z[rayIndex],gnormalz); 

			ray16.geomID[rayIndex] = tri_ptr->geomID();
			ray16.primID[rayIndex] = tri_ptr->primID();

#endif
			/* compact the stack if size of stack >= 2 */
			compactStack(stack_node_single,stack_dist_single,sindex,max_dist_xyz);
                      }
		  }
	      }
	    ray_tfar = select(valid0,ray16.tfar ,neg_inf);
	    continue;
	  }

	///////////////////////////////////////////////////////////////////////////////////////////////////////////////
	///////////////////////////////////////////////////////////////////////////////////////////////////////////////
	///////////////////////////////////////////////////////////////////////////////////////////////////////////////

	const unsigned int leaf_mask = BVH4I_LEAF_MASK;

        while (1)
        {
          /* test if this is a leaf node */
          if (unlikely(curNode.isLeaf(leaf_mask))) break;
          
          STAT3(normal.trav_nodes,1,popcnt(ray_tfar > curDist),16);
          const Node* __restrict__ const node = curNode.node(nodes);

	  prefetch<PFHINT_L1>((mic_f*)node + 0); 
	  prefetch<PFHINT_L1>((mic_f*)node + 1); 
          
          /* pop of next node */
          sptr_node--;
          sptr_dist--;
          curNode = *sptr_node; 
          curDist = *sptr_dist;
          

#pragma unroll(4)
          for (unsigned int i=0; i<4; i++)
          {
	    const NodeRef child = node->lower[i].child;

            const mic_f lclipMinX = msub(node->lower[i].x,rdir16.x,org_rdir16.x);
            const mic_f lclipMinY = msub(node->lower[i].y,rdir16.y,org_rdir16.y);
            const mic_f lclipMinZ = msub(node->lower[i].z,rdir16.z,org_rdir16.z);
            const mic_f lclipMaxX = msub(node->upper[i].x,rdir16.x,org_rdir16.x);
            const mic_f lclipMaxY = msub(node->upper[i].y,rdir16.y,org_rdir16.y);
            const mic_f lclipMaxZ = msub(node->upper[i].z,rdir16.z,org_rdir16.z);
	    
	    if (unlikely(i >=2 && child == BVH4i::invalidNode)) break;

            const mic_f lnearP = max(max(min(lclipMinX, lclipMaxX), min(lclipMinY, lclipMaxY)), min(lclipMinZ, lclipMaxZ));
            const mic_f lfarP  = min(min(max(lclipMinX, lclipMaxX), max(lclipMinY, lclipMaxY)), max(lclipMinZ, lclipMaxZ));
            const mic_m lhit   = max(lnearP,ray_tnear) <= min(lfarP,ray_tfar);   
	    const mic_f childDist = select(lhit,lnearP,inf);
            const mic_m m_child_dist = childDist < curDist;


            /* if we hit the child we choose to continue with that child if it 
               is closer than the current next child, or we push it onto the stack */
            if (likely(any(lhit)))
            {
              sptr_node++;
              sptr_dist++;

              /* push cur node onto stack and continue with hit child */
              if (any(m_child_dist))
              {
                *(sptr_node-1) = curNode;
                *(sptr_dist-1) = curDist; 
                curDist = childDist;
                curNode = child;
              }              
              /* push hit child onto stack*/
              else 
		{
		  *(sptr_node-1) = child;
		  *(sptr_dist-1) = childDist; 

#if defined(EXTENDED_PREFETCHING)
		    const char* __restrict__ const pnode = (char*)child.node(nodes);             
		    prefetch<PFHINT_L2>(pnode + 0);
		    prefetch<PFHINT_L2>(pnode + 64);
#endif
		}
              assert(sptr_node - stack_node < BVH4i::maxDepth);
            }	      
          }
#if SWITCH_ON_DOWN_TRAVERSAL == 1
	  const mic_m curUtil = ray_tfar > curDist;
	  if (unlikely(countbits(curUtil) <= BVH4i::hybridSIMDUtilSwitchThreshold))
	    {
	      *sptr_node++ = curNode;
	      *sptr_dist++ = curDist; 
	      goto pop;
	    }
#endif
        }
        
        /* return if stack is empty */
        if (unlikely(curNode == BVH4i::invalidNode)) break;
        
	const mic3f org = ray16.org;

        /* intersect leaf */
        const mic_m valid_leaf = ray_tfar > curDist;
        STAT3(normal.trav_leaves,1,popcnt(valid_leaf),16);

	unsigned int items; 
	const Triangle1* __restrict__ tris  = (Triangle1*) curNode.leaf(accel,items);

	const mic_f zero = mic_f::zero();
	const mic_f one  = mic_f::one();
	
	prefetch<PFHINT_L1>((mic_f*)tris +  0); 
	prefetch<PFHINT_L2>((mic_f*)tris +  1); 
	prefetch<PFHINT_L2>((mic_f*)tris +  2); 
	prefetch<PFHINT_L2>((mic_f*)tris +  3); 

	for (size_t i=0; i<items; i++,tris++) 
	  {
	    const Triangle1& tri = *tris;

	    prefetch<PFHINT_L1>(tris + 1 ); 

	    STAT3(normal.trav_prims,1,popcnt(valid_i),16);
        
	    /* load vertices and calculate edges */
	    const mic_f v0 = broadcast4to16f(&tri.v0);
	    const mic_f v1 = broadcast4to16f(&tri.v1);
	    const mic_f v2 = broadcast4to16f(&tri.v2);
	    

	    const mic_f e1 = v0-v1;
	    const mic_f e2 = v2-v0;

	    /* calculate denominator */
	    const mic3f _v0 = mic3f(swizzle<0>(v0),swizzle<1>(v0),swizzle<2>(v0));
	    const mic3f C =  _v0 - org;
	    
	    const mic3f Ng = mic3f(tri.Ng);
	    const mic_f den = dot(Ng,ray16.dir);

	    const mic_f rcp_den = rcp(den);

	    mic_m valid = valid_leaf;

#if defined(__BACKFACE_CULLING__)
	    
	    valid &= den > zero;
#endif

	    /* perform edge tests */
	    const mic3f R = -cross(C,ray16.dir);
	    const mic3f _e2(swizzle<0>(e2),swizzle<1>(e2),swizzle<2>(e2));
	    const mic_f u = dot(R,_e2)*rcp_den;
	    const mic3f _e1(swizzle<0>(e1),swizzle<1>(e1),swizzle<2>(e1));
	    const mic_f v = dot(R,_e1)*rcp_den;
	    valid = ge(valid,u,zero);
	    valid = ge(valid,v,zero);
	    valid = le(valid,u+v,one);

	    prefetch<PFHINT_L1EX>(&ray16.u);      
	    prefetch<PFHINT_L1EX>(&ray16.v);      
	    prefetch<PFHINT_L1EX>(&ray16.tfar);      


	    if (unlikely(none(valid))) continue;

	    const mic_f dot_C_Ng = dot(C,Ng);
	    const mic_f t = dot_C_Ng * rcp_den;
      
	    /* perform depth test */
	    valid = ge(valid, t,ray16.tnear);
	    valid = ge(valid,ray16.tfar,t);

	    const mic_i geomID = tri.geomID();
	    const mic_i primID = tri.primID();
	    prefetch<PFHINT_L1EX>(&ray16.geomID);      
	    prefetch<PFHINT_L1EX>(&ray16.primID);      
	    prefetch<PFHINT_L1EX>(&ray16.Ng.x);      
	    prefetch<PFHINT_L1EX>(&ray16.Ng.y);      
	    prefetch<PFHINT_L1EX>(&ray16.Ng.z);      

	    /* ray masking test */
#if defined(__USE_RAY_MASK__)
	    valid &= (tri.mask() & ray16.mask) != 0;
#endif
	    if (unlikely(none(valid))) continue;

            /* intersection filter test */
#if defined(__INTERSECTION_FILTER__)
            Geometry* geom = ((Scene*)bvh->geometry)->get(tri.geomID());
            if (unlikely(geom->hasIntersectionFilter16())) {
              runIntersectionFilter16(valid,geom,ray16,u,v,t,Ng,geomID,primID);
              continue;
            }
#endif

	    /* update hit information */
	    store16f(valid,(float*)&ray16.u,u);
	    store16f(valid,(float*)&ray16.v,v);
	    store16f(valid,(float*)&ray16.tfar,t);
	    store16i(valid,(float*)&ray16.geomID,geomID);
	    store16i(valid,(float*)&ray16.primID,primID);
	    store16f(valid,(float*)&ray16.Ng.x,Ng.x);
	    store16f(valid,(float*)&ray16.Ng.y,Ng.y);
	    store16f(valid,(float*)&ray16.Ng.z,Ng.z);
	  }

        ray_tfar = select(valid_leaf,ray16.tfar,ray_tfar);
      }
    }
    
    void BVH4iIntersector16Hybrid::occluded(mic_i* valid_i, BVH4i* bvh, Ray16& ray16)
    {
      /* allocate stack */
      __aligned(64) mic_f   stack_dist[3*BVH4i::maxDepth+1];
      __aligned(64) NodeRef stack_node[3*BVH4i::maxDepth+1];
      __aligned(64) NodeRef stack_node_single[3*BVH4i::maxDepth+1];

      /* load ray */
      const mic_m m_valid     = *(mic_i*)valid_i != mic_i(0);
      mic_m m_terminated      = !m_valid;
      const mic3f rdir16      = rcp_safe(ray16.dir);
      const mic3f org_rdir16  = ray16.org * rdir16;
      mic_f ray_tnear         = select(m_valid,ray16.tnear,pos_inf);
      mic_f ray_tfar          = select(m_valid,ray16.tfar ,neg_inf);
      const mic_f inf         = mic_f(pos_inf);

      
      /* push root node */
      stack_node[0] = BVH4i::invalidNode;
      stack_dist[0] = inf;
      stack_node[1] = bvh->root;
      stack_dist[1] = ray_tnear; 
      NodeRef* __restrict__ sptr_node = stack_node + 2;
      mic_f*   __restrict__ sptr_dist = stack_dist + 2;
      
      const Node      * __restrict__ nodes = (Node     *)bvh->nodePtr();
      const Triangle1 * __restrict__ accel = (Triangle1*)bvh->triPtr();

      while (1) pop_occluded:
      {
	const mic_m m_active = !m_terminated;

        /* pop next node from stack */
        NodeRef curNode = *(sptr_node-1);
        mic_f curDist   = *(sptr_dist-1);
        sptr_node--;
        sptr_dist--;
	const mic_m m_stackDist = gt(m_active,ray_tfar,curDist);

	/* stack emppty ? */
        if (unlikely(curNode == BVH4i::invalidNode))  break;
        
        /* cull node if behind closest hit point */
        if (unlikely(none(m_stackDist))) continue;        

	/* switch to single ray mode */
        if (unlikely(countbits(m_stackDist) <= BVH4i::hybridSIMDUtilSwitchThreshold)) 
	  {
	    stack_node_single[0] = BVH4i::invalidNode;

	    /* traverse single ray */	  	  
	    long rayIndex = -1;
	    while((rayIndex = bitscan64(rayIndex,m_stackDist)) != BITSCAN_NO_BIT_SET_64) 
	      {	    
		stack_node_single[1] = curNode;
		size_t sindex = 2;

		const mic_f org_xyz      = loadAOS4to16f(rayIndex,ray16.org.x,ray16.org.y,ray16.org.z);
		const mic_f dir_xyz      = loadAOS4to16f(rayIndex,ray16.dir.x,ray16.dir.y,ray16.dir.z);
		const mic_f rdir_xyz     = loadAOS4to16f(rayIndex,rdir16.x,rdir16.y,rdir16.z);
		const mic_f org_rdir_xyz = org_xyz * rdir_xyz;
		const mic_f min_dist_xyz = broadcast1to16f(&ray16.tnear[rayIndex]);
		const mic_f max_dist_xyz = broadcast1to16f(&ray16.tfar[rayIndex]);
		const unsigned int leaf_mask = BVH4I_LEAF_MASK;
		//const mic_m m7777 = 0x7777; // M_LANE_7777;
		//const mic_m m_rdir0 = lt(m7777,rdir_xyz,mic_f::zero());
		//const mic_m m_rdir1 = ge(m7777,rdir_xyz,mic_f::zero());

		while (1) 
		  {
		    NodeRef curNode = stack_node_single[sindex-1];
		    sindex--;
            
		    traverse_single_occluded(curNode,
					     sindex,
					     rdir_xyz,
					     org_rdir_xyz,
					     min_dist_xyz,
					     max_dist_xyz,
					     stack_node_single,
					     nodes,
					     leaf_mask);	    

		    /* return if stack is empty */
		    if (unlikely(curNode == BVH4i::invalidNode)) break;

		    const mic_f zero = mic_f::zero();

		    /* intersect one ray against four triangles */

		    const Triangle1* tptr  = (Triangle1*) curNode.leaf(accel);
		    prefetch<PFHINT_L1>(tptr + 3);
		    prefetch<PFHINT_L1>(tptr + 2);
		    prefetch<PFHINT_L1>(tptr + 1);
		    prefetch<PFHINT_L1>(tptr + 0); 

		    const mic_i and_mask = broadcast4to16i(zlc4);
	      
		    const mic_f v0 = gather_4f_zlc(and_mask,
						   (float*)&tptr[0].v0,
						   (float*)&tptr[1].v0,
						   (float*)&tptr[2].v0,
						   (float*)&tptr[3].v0);
	      
		    const mic_f v1 = gather_4f_zlc(and_mask,
						   (float*)&tptr[0].v1,
						   (float*)&tptr[1].v1,
						   (float*)&tptr[2].v1,
						   (float*)&tptr[3].v1);
	      
		    const mic_f v2 = gather_4f_zlc(and_mask,
						   (float*)&tptr[0].v2,
						   (float*)&tptr[1].v2,
						   (float*)&tptr[2].v2,
						   (float*)&tptr[3].v2);

		    const mic_f e1 = v1 - v0;
		    const mic_f e2 = v0 - v2;	     
		    const mic_f normal = lcross_zxy(e1,e2);
		    const mic_f org = v0 - org_xyz;
		    const mic_f odzxy = msubr231(org * swizzle(dir_xyz,_MM_SWIZ_REG_DACB), dir_xyz, swizzle(org,_MM_SWIZ_REG_DACB));
		    const mic_f den = ldot3_zxy(dir_xyz,normal);	      
		    const mic_f rcp_den = rcp(den);
		    const mic_f uu = ldot3_zxy(e2,odzxy); 
		    const mic_f vv = ldot3_zxy(e1,odzxy); 
		    const mic_f u = uu * rcp_den;
		    const mic_f v = vv * rcp_den;

#if defined(__BACKFACE_CULLING__)
		    const mic_m m_init = (mic_m)0x1111 & (den > zero);
#else
		    const mic_m m_init = 0x1111;
#endif

		    const mic_m valid_u = ge(m_init,u,zero);
		    const mic_m valid_v = ge(valid_u,v,zero);
		    const mic_m m_aperture = le(valid_v,u+v,mic_f::one()); 

		    const mic_f nom = ldot3_zxy(org,normal);
		    const mic_f t = rcp_den*nom;

		    if (unlikely(none(m_aperture))) continue;

		    mic_m m_final  = lt(lt(m_aperture,min_dist_xyz,t),t,max_dist_xyz);

#if defined(__USE_RAY_MASK__)
		    const mic_i rayMask(ray16.mask[rayIndex]);
		    const mic_i triMask = swDDDD(gather16i_4i_align(&tptr[0].v2,&tptr[1].v2,&tptr[2].v2,&tptr[3].v2));
		    const mic_m m_ray_mask = (rayMask & triMask) != mic_i::zero();
		    m_final &= m_ray_mask;	      
#endif

#if defined(__INTERSECTION_FILTER__) 
              
		    /* did the ray hit one of the four triangles? */
		    while (any(m_final)) 
		      {
			const mic_f temp_t  = select(m_final,t,max_dist_xyz);
			const mic_f min_dist = vreduce_min(temp_t);
			const mic_m m_dist = eq(min_dist,temp_t);
			const size_t vecIndex = bitscan(toInt(m_dist));
			const size_t triIndex = vecIndex >> 2;
			const Triangle1  *__restrict__ tri_ptr = tptr + triIndex;
			const mic_m m_tri = m_dist^(m_dist & (mic_m)((unsigned int)m_dist - 1));
			const mic_f gnormalx = mic_f(tri_ptr->Ng.x);
			const mic_f gnormaly = mic_f(tri_ptr->Ng.y);
			const mic_f gnormalz = mic_f(tri_ptr->Ng.z);
			const int geomID = tri_ptr->geomID();
			const int primID = tri_ptr->primID();                
			Geometry* geom = ((Scene*)bvh->geometry)->get(geomID);
			if (likely(!geom->hasOcclusionFilter16())) break;
                
			if (runOcclusionFilter16(geom,ray16,rayIndex,u,v,min_dist,gnormalx,gnormaly,gnormalz,m_tri,geomID,primID)) 
			  break;

			m_final ^= m_tri; /* clear bit */
		      }
#endif

		    /* did the ray hit one of the four triangles? */
		    if (unlikely(any(m_final)))
		      {
			m_terminated |= toMask(mic_m::shift1[rayIndex]);
			break;
		      }
		  }
		if (unlikely(all(m_terminated))) 
		  {
		    store16i(m_valid,&ray16.geomID,mic_i::zero());
		    return;
		  }      
	      }
	    continue;
	  }

	///////////////////////////////////////////////////////////////////////////////////////////////////////////////
	///////////////////////////////////////////////////////////////////////////////////////////////////////////////
	///////////////////////////////////////////////////////////////////////////////////////////////////////////////

	const unsigned int leaf_mask = BVH4I_LEAF_MASK;

        while (1)
        {
          /* test if this is a leaf node */
          if (unlikely(curNode.isLeaf(leaf_mask))) break;
          
          STAT3(shadow.trav_nodes,1,popcnt(ray_tfar > curDist),16);
          const Node* __restrict__ const node = curNode.node(nodes);
          
	  prefetch<PFHINT_L1>((char*)node + 0);
	  prefetch<PFHINT_L1>((char*)node + 64);

          /* pop of next node */
          curNode = *(sptr_node-1); 
          curDist = *(sptr_dist-1);
          sptr_node--;
          sptr_dist--;

	  mic_m m_curUtil = gt(ray_tfar,curDist);
          
#pragma unroll(4)
          for (size_t i=0; i<4; i++)
	    {
	      const NodeRef child = node->lower[i].child;
            
	      const mic_f lclipMinX = msub(node->lower[i].x,rdir16.x,org_rdir16.x);
	      const mic_f lclipMinY = msub(node->lower[i].y,rdir16.y,org_rdir16.y);
	      const mic_f lclipMinZ = msub(node->lower[i].z,rdir16.z,org_rdir16.z);
	      const mic_f lclipMaxX = msub(node->upper[i].x,rdir16.x,org_rdir16.x);
	      const mic_f lclipMaxY = msub(node->upper[i].y,rdir16.y,org_rdir16.y);
	      const mic_f lclipMaxZ = msub(node->upper[i].z,rdir16.z,org_rdir16.z);	    

	      if (unlikely(i >=2 && child == BVH4i::invalidNode)) break;

	      const mic_f lnearP = max(max(min(lclipMinX, lclipMaxX), min(lclipMinY, lclipMaxY)), min(lclipMinZ, lclipMaxZ));
	      const mic_f lfarP  = min(min(max(lclipMinX, lclipMaxX), max(lclipMinY, lclipMaxY)), max(lclipMinZ, lclipMaxZ));
	      const mic_m lhit   = max(lnearP,ray_tnear) <= min(lfarP,ray_tfar);      
	      const mic_f childDist = select(lhit,lnearP,inf);
	      const mic_m m_child_dist = childDist < curDist;

            
	      /* if we hit the child we choose to continue with that child if it 
		 is closer than the current next child, or we push it onto the stack */
	      if (likely(any(lhit)))
		{
		  sptr_node++;
		  sptr_dist++;

              
		  /* push cur node onto stack and continue with hit child */
		  if (any(m_child_dist))
		    {
		      *(sptr_node-1) = curNode;
		      *(sptr_dist-1) = curDist; 
		      curDist = childDist;
		      curNode = child;
		      m_curUtil = gt(ray_tfar,curDist);
		    }
              
		  /* push hit child onto stack*/
		  else {
		    *(sptr_node-1) = child;
		    *(sptr_dist-1) = childDist; 

#if defined(EXTENDED_PREFETCHING)
		    const char* __restrict__ const pnode = (char*)child.node(nodes);             
		    prefetch<PFHINT_L2>(pnode + 0);
		    prefetch<PFHINT_L2>(pnode + 64);
#endif
		  }
		  assert(sptr_node - stack_node < BVH4i::maxDepth);
		}	      
	    }


#if SWITCH_ON_DOWN_TRAVERSAL == 1
	  const unsigned int curUtil = countbits(m_curUtil);
	  if (unlikely(curUtil <= BVH4i::hybridSIMDUtilSwitchThreshold))
	    {
	      *sptr_node++ = curNode;
	      *sptr_dist++ = curDist; 
	      goto pop_occluded;
	    }
#endif

        }
        
        /* return if stack is empty */
        if (unlikely(curNode == BVH4i::invalidNode)) break;
        
        /* intersect leaf */
        mic_m valid_leaf = gt(m_active,ray_tfar,curDist);
        STAT3(shadow.trav_leaves,1,popcnt(valid_leaf),16);
        unsigned int items; const Triangle1* tris  = (Triangle1*) curNode.leaf(accel,items);

	prefetch<PFHINT_NT>((mic_f*)tris +  0); 
	prefetch<PFHINT_L2>((mic_f*)tris +  1); 
	prefetch<PFHINT_L2>((mic_f*)tris +  2); 
	prefetch<PFHINT_L2>((mic_f*)tris +  3); 

	mic_m valid0 = valid_leaf;

	const mic3f org = ray16.org;
	const mic3f dir = ray16.dir;

	const mic_f zero = mic_f::zero();
	const mic_f one  = mic_f::one();

     
	for (size_t i=0; i<items; i++,tris++) 
	  {
	    STAT3(shadow.trav_prims,1,popcnt(valid0),16);

	    prefetch<PFHINT_NT>((mic_f*)tris + 1); 

	    mic_m valid = valid0;
	    const Triangle1& tri = *tris;
        
	    /* load vertices and calculate edges */
	    const mic_f v0 = broadcast4to16f(&tri.v0);
	    const mic_f v1 = broadcast4to16f(&tri.v1);
	    const mic_f v2 = broadcast4to16f(&tri.v2);
	    const mic_f e1 = v0-v1;
	    const mic_f e2 = v2-v0;


        
	    /* calculate denominator */
	    const mic3f _v0 = mic3f(swizzle<0>(v0),swizzle<1>(v0),swizzle<2>(v0));
	    const mic3f C =  _v0 - org;

	    const mic_f Ng = broadcast4to16f(&tri.Ng);
	    const mic3f _Ng = mic3f(swizzle<0>(Ng),swizzle<1>(Ng),swizzle<2>(Ng));
	    const mic_f den = dot(dir,_Ng);

#if defined(__BACKFACE_CULLING__)
	    valid &= den > zero;
#endif
	    const mic_f rcp_den = rcp(den);
	    const mic3f R = cross(dir,C);
	    const mic3f _e1(swizzle<0>(e1),swizzle<1>(e1),swizzle<2>(e1));
	    const mic_f u = dot(R,_e1)*rcp_den;
	    const mic3f _e2(swizzle<0>(e2),swizzle<1>(e2),swizzle<2>(e2));
	    const mic_f v = dot(R,_e2)*rcp_den;
	    valid = ge(valid,u,zero);
	    valid = ge(valid,v,zero);
	    valid = le(valid,u+v,one); 
	    const mic_f t = dot(C,_Ng) * rcp_den;
	    evictL1(tris);

	    if (unlikely(none(valid))) continue;
      
	    /* perform depth test */
	    valid = ge(valid, t,ray16.tnear);
	    valid = ge(valid,ray16.tfar,t);

	    /* ray masking test */
#if defined(__USE_RAY_MASK__)
	    valid &= (tri.mask() & ray16.mask) != 0;
#endif
	    if (unlikely(none(valid))) continue;

            /* intersection filter test */
#if defined(__INTERSECTION_FILTER__)
            const int geomID = tri.geomID();
            Geometry* geom = ((Scene*)bvh->geometry)->get(geomID);
            if (unlikely(geom->hasOcclusionFilter16()))
              valid = runOcclusionFilter16(valid,geom,ray16,u,v,t,Ng,geomID,tri.primID());
#endif

	    /* update occlusion */
	    valid0 &= !valid;
	    if (unlikely(none(valid0))) break;
	  }
	m_terminated |= valid_leaf & (!valid0);	

        ray_tfar = select(m_terminated,neg_inf,ray_tfar);
        if (unlikely(all(m_terminated))) break;
      }
      store16i(m_valid & m_terminated,&ray16.geomID,mic_i::zero());
    }
    
    DEFINE_INTERSECTOR16    (BVH4iTriangle1Intersector16HybridMoeller, BVH4iIntersector16Hybrid);
  }
}
