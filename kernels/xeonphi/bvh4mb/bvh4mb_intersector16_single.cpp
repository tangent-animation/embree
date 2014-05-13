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

#include "bvh4mb_intersector16_single.h"
#include "bvh4mb_traversal.h"
#include "bvh4mb_leaf_intersector.h"

namespace embree
{
  namespace isa
  {

    static unsigned int BVH4I_LEAF_MASK = BVH4i::leaf_mask; // needed due to compiler efficiency bug

    static __aligned(64) int zlc4[4] = {0xffffffff,0xffffffff,0xffffffff,0};

    template<typename LeafIntersector>
    void BVH4mbIntersector16Single<LeafIntersector>::intersect(mic_i* valid_i, BVH4mb* bvh, Ray16& ray16)
    {      
      /* near and node stack */
      __aligned(64) float   stack_dist[3*BVH4i::maxDepth+1];
      __aligned(64) NodeRef stack_node[3*BVH4i::maxDepth+1];

      /* setup */
      const mic_m m_valid    = *(mic_i*)valid_i != mic_i(0);
      const mic3f rdir16     = rcp_safe(ray16.dir);
      const mic_f inf        = mic_f(pos_inf);
      const mic_f zero       = mic_f::zero();
      store16f(stack_dist,inf);

      const Node               * __restrict__ nodes = (Node     *)bvh->nodePtr();
      const BVH4mb::Triangle01 * __restrict__ accel = (BVH4mb::Triangle01 *)bvh->triPtr();

      stack_node[0] = BVH4i::invalidNode;
      long rayIndex = -1;
      while((rayIndex = bitscan64(rayIndex,toInt(m_valid))) != BITSCAN_NO_BIT_SET_64)	    
        {
	  stack_node[1] = bvh->root;
	  size_t sindex = 2;

	  const mic_f org_xyz      = loadAOS4to16f(rayIndex,ray16.org.x,ray16.org.y,ray16.org.z);
	  const mic_f dir_xyz      = loadAOS4to16f(rayIndex,ray16.dir.x,ray16.dir.y,ray16.dir.z);
	  const mic_f rdir_xyz     = loadAOS4to16f(rayIndex,rdir16.x,rdir16.y,rdir16.z);
	  const mic_f org_rdir_xyz = org_xyz * rdir_xyz;
	  const mic_f min_dist_xyz = broadcast1to16f(&ray16.tnear[rayIndex]);
	  mic_f       max_dist_xyz = broadcast1to16f(&ray16.tfar[rayIndex]);
	  const mic_f time         = broadcast1to16f(&ray16.time[rayIndex]);
	      
	  
	  const unsigned int leaf_mask = BVH4I_LEAF_MASK;
	  const mic_m m7777 = 0x7777; 
	  const mic_m m_rdir0 = lt(m7777,rdir_xyz,mic_f::zero());
	  const mic_m m_rdir1 = ge(m7777,rdir_xyz,mic_f::zero());

	  while (1)
	    {
	      NodeRef curNode = stack_node[sindex-1];
	      sindex--;
            
	      const mic_f one_time = (mic_f::one() - time);

	      while (1) 
		{
		  /* test if this is a leaf node */
		  if (unlikely(curNode.isLeaf(leaf_mask))) break;
        
		  const Node* __restrict__ const node = curNode.node(nodes);
		  const float* __restrict const plower = (float*)node->lower;
		  const float* __restrict const pupper = (float*)node->upper;

		  prefetch<PFHINT_L1>((char*)node + 0*64);
		  prefetch<PFHINT_L1>((char*)node + 1*64);
		  prefetch<PFHINT_L1>((char*)node + 2*64);
		  prefetch<PFHINT_L1>((char*)node + 3*64);

		  const BVH4mb::Node* __restrict__ const nodeMB = (BVH4mb::Node*)node;
		  const mic_f lower = one_time  * load16f((float*)nodeMB->lower) + time * load16f((float*)nodeMB->lower_t1);
		  const mic_f upper = one_time  * load16f((float*)nodeMB->upper) + time * load16f((float*)nodeMB->upper_t1);
		  
        
		  /* intersect single ray with 4 bounding boxes */
		  mic_f tLowerXYZ = select(m7777,rdir_xyz,min_dist_xyz);
		  mic_f tUpperXYZ = select(m7777,rdir_xyz,max_dist_xyz);

		  tLowerXYZ = mask_msub(m_rdir1,tLowerXYZ,lower,org_rdir_xyz);
		  tUpperXYZ = mask_msub(m_rdir0,tUpperXYZ,lower,org_rdir_xyz);

		  tLowerXYZ = mask_msub(m_rdir0,tLowerXYZ,upper,org_rdir_xyz);
		  tUpperXYZ = mask_msub(m_rdir1,tUpperXYZ,upper,org_rdir_xyz);

		  mic_m hitm = ~m7777; 
		  const mic_f tLower = tLowerXYZ;
		  const mic_f tUpper = tUpperXYZ;

		  sindex--;

		  curNode = stack_node[sindex]; // early pop of next node

		  const Node* __restrict__ const next = curNode.node(nodes);
		  prefetch<PFHINT_L2>((char*)next + 0);
		  prefetch<PFHINT_L2>((char*)next + 64);

		  const mic_f tNear = vreduce_max4(tLower);
		  const mic_f tFar  = vreduce_min4(tUpper);  
		  hitm = le(hitm,tNear,tFar);
		  const mic_f tNear_pos = select(hitm,tNear,inf);


		  /* if no child is hit, continue with early popped child */
		  if (unlikely(none(hitm))) continue;
		  sindex++;
        
		  const unsigned long hiti = toInt(hitm);
		  const unsigned long pos_first = bitscan64(hiti);
		  const unsigned long num_hitm = countbits(hiti); 
        
		  /* if a single child is hit, continue with that child */
		  curNode = ((unsigned int *)plower)[pos_first];
		  if (likely(num_hitm == 1)) continue;
        
		  /* if two children are hit, push in correct order */
		  const unsigned long pos_second = bitscan64(pos_first,hiti);
		  if (likely(num_hitm == 2))
		    {
		      const unsigned int dist_first  = ((unsigned int*)&tNear)[pos_first];
		      const unsigned int dist_second = ((unsigned int*)&tNear)[pos_second];
		      const unsigned int node_first  = curNode;
		      const unsigned int node_second = ((unsigned int*)plower)[pos_second];
          
		      if (dist_first <= dist_second)
			{
			  stack_node[sindex] = node_second;
			  ((unsigned int*)stack_dist)[sindex] = dist_second;                      
			  sindex++;
			  assert(sindex < 3*BVH4i::maxDepth+1);
			  continue;
			}
		      else
			{
			  stack_node[sindex] = curNode;
			  ((unsigned int*)stack_dist)[sindex] = dist_first;
			  curNode = node_second;
			  sindex++;
			  assert(sindex < 3*BVH4i::maxDepth+1);
			  continue;
			}
		    }
        
		  /* continue with closest child and push all others */
		  const mic_f min_dist = set_min_lanes(tNear_pos);
		  const unsigned int old_sindex = sindex;
		  sindex += countbits(hiti) - 1;
		  assert(sindex < 3*BVH4i::maxDepth+1);
		  const mic_i plower_node = load16i((int*)plower);
        
		  const mic_m closest_child = eq(hitm,min_dist,tNear);
		  const unsigned long closest_child_pos = bitscan64(closest_child);
		  const mic_m m_pos = andn(hitm,andn(closest_child,(mic_m)((unsigned int)closest_child - 1)));
		  curNode = ((unsigned int*)plower)[closest_child_pos];
		  compactustore16f(m_pos,&stack_dist[old_sindex],tNear);
		  compactustore16i(m_pos,&stack_node[old_sindex],plower_node);
		}
	  
	    

	      /* return if stack is empty */
	      if (unlikely(curNode == BVH4i::invalidNode)) break;


	      /* intersect one ray against four triangles */

	      //////////////////////////////////////////////////////////////////////////////////////////////////

	      const bool hit = LeafIntersector::intersect(curNode,
							  rayIndex,
							  dir_xyz,
							  org_xyz,
							  min_dist_xyz,
							  max_dist_xyz,
							  ray16,
							  accel,
							  (Scene*)bvh->geometry);
									   
	      if (hit)
		compactStack(stack_node,stack_dist,sindex,max_dist_xyz);

	    }	  
	}
    }
    
    template<typename LeafIntersector>
    void BVH4mbIntersector16Single<LeafIntersector>::occluded(mic_i* valid_i, BVH4mb* bvh, Ray16& ray16)
    {
      /* near and node stack */
      __aligned(64) NodeRef stack_node[3*BVH4i::maxDepth+1];

      /* setup */
      const mic_m m_valid     = *(mic_i*)valid_i != mic_i(0);
      const mic3f rdir16      = rcp_safe(ray16.dir);
      mic_m m_terminated      = !m_valid;
      const mic_f inf         = mic_f(pos_inf);
      const mic_f zero        = mic_f::zero();

      const Node               * __restrict__ nodes = (Node     *)bvh->nodePtr();
      const BVH4mb::Triangle01 * __restrict__ accel = (BVH4mb::Triangle01 *)bvh->triPtr();

      stack_node[0] = BVH4i::invalidNode;

      long rayIndex = -1;
      while((rayIndex = bitscan64(rayIndex,toInt(m_valid))) != BITSCAN_NO_BIT_SET_64)	    
        {
	  stack_node[1] = bvh->root;
	  size_t sindex = 2;

	  const mic_f org_xyz      = loadAOS4to16f(rayIndex,ray16.org.x,ray16.org.y,ray16.org.z);
	  const mic_f dir_xyz      = loadAOS4to16f(rayIndex,ray16.dir.x,ray16.dir.y,ray16.dir.z);
	  const mic_f rdir_xyz     = loadAOS4to16f(rayIndex,rdir16.x,rdir16.y,rdir16.z);
	  const mic_f org_rdir_xyz = org_xyz * rdir_xyz;
	  const mic_f min_dist_xyz = broadcast1to16f(&ray16.tnear[rayIndex]);
	  const mic_f max_dist_xyz = broadcast1to16f(&ray16.tfar[rayIndex]);
	  const mic_f time         = broadcast1to16f(&ray16.time[rayIndex]);

	  const unsigned int leaf_mask = BVH4I_LEAF_MASK;
	  const mic_m m7777 = 0x7777; 
	  const mic_m m_rdir0 = lt(m7777,rdir_xyz,mic_f::zero());
	  const mic_m m_rdir1 = ge(m7777,rdir_xyz,mic_f::zero());

	  while (1)
	    {
	      NodeRef curNode = stack_node[sindex-1];
	      sindex--;

	      const mic_f one_time = (mic_f::one() - time);
            
	      while (1) 
		{
		  /* test if this is a leaf node */
		  if (unlikely(curNode.isLeaf(leaf_mask))) break;
        
		  const Node* __restrict__ const node = curNode.node(nodes);
		  const float* __restrict const plower = (float*)node->lower;
		  const float* __restrict const pupper = (float*)node->upper;

		  prefetch<PFHINT_L1>((char*)node + 0*64);
		  prefetch<PFHINT_L1>((char*)node + 1*64);
		  prefetch<PFHINT_L1>((char*)node + 2*64);
		  prefetch<PFHINT_L1>((char*)node + 3*64);

		  const BVH4mb::Node* __restrict__ const nodeMB = (BVH4mb::Node*)node;
		  const mic_f lower = one_time  * load16f((float*)nodeMB->lower) + time * load16f((float*)nodeMB->lower_t1);
		  const mic_f upper = one_time  * load16f((float*)nodeMB->upper) + time * load16f((float*)nodeMB->upper_t1);
        
		  /* intersect single ray with 4 bounding boxes */
		  mic_f tLowerXYZ = select(m7777,rdir_xyz,min_dist_xyz);
		  mic_f tUpperXYZ = select(m7777,rdir_xyz,max_dist_xyz);

		  tLowerXYZ = mask_msub(m_rdir1,tLowerXYZ,lower,org_rdir_xyz);
		  tUpperXYZ = mask_msub(m_rdir0,tUpperXYZ,lower,org_rdir_xyz);

		  tLowerXYZ = mask_msub(m_rdir0,tLowerXYZ,upper,org_rdir_xyz);
		  tUpperXYZ = mask_msub(m_rdir1,tUpperXYZ,upper,org_rdir_xyz);

		  mic_m hitm = ~m7777; 
		  const mic_f tLower = tLowerXYZ;
		  const mic_f tUpper = tUpperXYZ;

		  const Node* __restrict__ const next = curNode.node(nodes);
		  prefetch<PFHINT_L2>((char*)next + 0);
		  prefetch<PFHINT_L2>((char*)next + 64);

		  sindex--;
		  const mic_f tNear = vreduce_max4(tLower);
		  const mic_f tFar  = vreduce_min4(tUpper);  
		  hitm = le(hitm,tNear,tFar);
		  const mic_f tNear_pos = select(hitm,tNear,inf);

		  curNode = stack_node[sindex]; // early pop of next node

		  /* if no child is hit, continue with early popped child */
		  if (unlikely(none(hitm))) continue;
		  sindex++;
        
		  const unsigned long hiti = toInt(hitm);
		  const unsigned long pos_first = bitscan64(hiti);
		  const unsigned long num_hitm = countbits(hiti); 
        
		  /* if a single child is hit, continue with that child */
		  curNode = ((unsigned int *)plower)[pos_first];
		  if (likely(num_hitm == 1)) continue;
        
		  /* if two children are hit, push in correct order */
		  const unsigned long pos_second = bitscan64(pos_first,hiti);
		  if (likely(num_hitm == 2))
		    {
		      const unsigned int dist_first  = ((unsigned int*)&tNear)[pos_first];
		      const unsigned int dist_second = ((unsigned int*)&tNear)[pos_second];
		      const unsigned int node_first  = curNode;
		      const unsigned int node_second = ((unsigned int*)plower)[pos_second];
          
		      if (dist_first <= dist_second)
			{
			  stack_node[sindex] = node_second;
			  sindex++;
			  assert(sindex < 3*BVH4i::maxDepth+1);
			  continue;
			}
		      else
			{
			  stack_node[sindex] = curNode;
			  curNode = node_second;
			  sindex++;
			  assert(sindex < 3*BVH4i::maxDepth+1);
			  continue;
			}
		    }
        
		  /* continue with closest child and push all others */
		  const mic_f min_dist = set_min_lanes(tNear_pos);
		  const unsigned int old_sindex = sindex;
		  sindex += countbits(hiti) - 1;
		  assert(sindex < 3*BVH4i::maxDepth+1);
        
		  const mic_m closest_child = eq(hitm,min_dist,tNear);
		  const unsigned long closest_child_pos = bitscan64(closest_child);
		  const mic_m m_pos = andn(hitm,andn(closest_child,(mic_m)((unsigned int)closest_child - 1)));
		  const mic_i plower_node = load16i((int*)plower);
		  curNode = ((unsigned int*)plower)[closest_child_pos];
		  compactustore16i(m_pos,&stack_node[old_sindex],plower_node);
		}
	  
	    

	      /* return if stack is empty */
	      if (unlikely(curNode == BVH4i::invalidNode)) break;


	      /* intersect one ray against four triangles */

	      //////////////////////////////////////////////////////////////////////////////////////////////////

	      const bool hit = LeafIntersector::occluded(curNode,
							 rayIndex,
							 dir_xyz,
							 org_xyz,
							 min_dist_xyz,
							 max_dist_xyz,
							 ray16,
							 m_terminated,
							 accel,
							 (Scene*)bvh->geometry);

	      if (unlikely(hit)) break;
	    }

	  if (unlikely(all(toMask(m_terminated)))) break;
	}


      store16i(m_valid & m_terminated,&ray16.geomID,0);
    }
    
    DEFINE_INTERSECTOR16    (BVH4mbTriangle1Intersector16SingleMoeller, BVH4mbIntersector16Single<Triangle1mbLeafIntersector>);

  }
}
