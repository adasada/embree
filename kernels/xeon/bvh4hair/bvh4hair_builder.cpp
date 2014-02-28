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

#include "bvh4hair.h"
#include "bvh4hair_builder.h"
#include "bvh4hair_statistics.h"
#include "common/scene_bezier_curves.h"

#define ENABLE_OBJECT_SPLITS 1
#define ENABLE_SUBDIV_SPLITS 0
#define ENABLE_SPATIAL_SPLITS 0
#define ENABLE_STRAND_SPLITS 1
#define ENABLE_ALIGNED_SPLITS 1
#define ENABLE_UNALIGNED_SPLITS 1
#define ENABLE_PRE_SUBDIVISION 1

namespace embree
{
#if BVH4HAIR_NAVIGATION
  extern BVH4Hair::NodeRef rootNode;
  extern BVH4Hair::NodeRef naviNode;
  extern std::vector<BVH4Hair::NodeRef> naviStack;
#endif

  BVH4HairBuilder::BVH4HairBuilder (BVH4Hair* bvh, Scene* scene)
    : scene(scene), minLeafSize(1), maxLeafSize(inf), bvh(bvh)
  {
    if (BVH4Hair::maxLeafBlocks < this->maxLeafSize) 
      this->maxLeafSize = BVH4Hair::maxLeafBlocks;
  }

  BVH4HairBuilder::~BVH4HairBuilder () {
  }

  void BVH4HairBuilder::build(size_t threadIndex, size_t threadCount) 
  {
    /* fast path for empty BVH */
    size_t numPrimitives = scene->numCurves;
    bvh->init(3*numPrimitives); // FIXME: 2x for spatial splits
    if (numPrimitives == 0) return;
    numGeneratedPrims = 0;
    numAlignedObjectSplits = 0;
    numAlignedSubdivObjectSplits = 0;
    numAlignedSpatialSplits = 0;
    numUnalignedObjectSplits = 0;
    numUnalignedSubdivObjectSplits = 0;
    numUnalignedSpatialSplits = 0;
    numStrandSplits = 0;
    numFallbackSplits = 0;
    
    double t0 = 0.0;
    if (g_verbose >= 2) {
      std::cout << "building BVH4Hair<Bezier1> ..." << std::flush;
      t0 = getSeconds();
    }

    size_t N = 0;
    float r = 0;

    /* create initial curve list */
    BBox3fa bounds = empty;
    curves.reserve(3*numPrimitives+100); // FIXME: 2x for spatial splits
    for (size_t i=0; i<scene->size(); i++) 
    {
      Geometry* geom = scene->get(i);
      if (geom->type != BEZIER_CURVES) continue;
      if (!geom->isEnabled()) continue;
      BezierCurves* set = (BezierCurves*) geom;

      for (size_t j=0; j<set->numCurves; j++) {
        const int ofs = set->curve(j);
        const Vec3fa& p0 = set->vertex(ofs+0);
        const Vec3fa& p1 = set->vertex(ofs+1);
        const Vec3fa& p2 = set->vertex(ofs+2);
        const Vec3fa& p3 = set->vertex(ofs+3);
        const Bezier1 bezier(p0,p1,p2,p3,0,1,i,j);
        bounds.extend(bezier.bounds());
        curves.push_back(bezier);
      }
    }
    
    /* subdivide very curved hair segments */
#if ENABLE_PRE_SUBDIVISION
    //subdivide(0.01f);
    //subdivide(0.1f);
    //subdivide(0.25f);
    subdivide3();
#endif
    bvh->numPrimitives = curves.size();
    bvh->numVertices = 0;

    /* start recursive build */
    size_t begin = 0, end = curves.size();
    bvh->root = recurse(threadIndex,0,begin,end,computeAlignedBounds(&curves[0],begin,end,LinearSpace3fa(one)));
    bvh->bounds = bounds;
    NAVI(naviNode = bvh->root);
    NAVI(rootNode = bvh->root);
    NAVI(naviStack.push_back(bvh->root));

    if (g_verbose >= 2) {
      double t1 = getSeconds();
      std::cout << " [DONE]" << std::endl;
      std::cout << "  dt = " << 1000.0f*(t1-t0) << "ms, perf = " << 1E-6*double(numPrimitives)/(t1-t0) << " Mprim/s" << std::endl;
      PRINT(numAlignedObjectSplits);
      PRINT(numAlignedSpatialSplits);
      PRINT(numAlignedSubdivObjectSplits);
      PRINT(numUnalignedObjectSplits);
      PRINT(numUnalignedSpatialSplits);
      PRINT(numUnalignedSubdivObjectSplits);
      PRINT(numStrandSplits);
      PRINT(numFallbackSplits);
      std::cout << BVH4HairStatistics(bvh).str();
    }
  }

  void BVH4HairBuilder::subdivide(float ratio)
  {
    if (g_verbose >= 2) 
      std::cout << std::endl << "  before subdivision: " << 1E-6*float(curves.size()) << " M curves" << std::endl;

    for (ssize_t i=0; i<curves.size(); i++)
    {
      /* calculate axis to align bounds */
      const Vec3fa axis = curves[i].p3-curves[i].p0;
      const float len = length(axis);
      if (len == 0.0f) continue; // FIXME: could still need subdivision
      
      /* test if we should subdivide */
      const LinearSpace3fa space = frame(axis/len).transposed();
      BBox3fa bounds = empty;
      const Vec3fa p0 = xfmPoint(space,curves[i].p0); bounds.extend(p0);
      const Vec3fa p1 = xfmPoint(space,curves[i].p1); bounds.extend(p1);
      const Vec3fa p2 = xfmPoint(space,curves[i].p2); bounds.extend(p2);
      const Vec3fa p3 = xfmPoint(space,curves[i].p3); bounds.extend(p3);
      const float r0 = curves[i].p0.w;
      const float r1 = curves[i].p1.w;
      const float r2 = curves[i].p2.w;
      const float r3 = curves[i].p3.w;
      bounds = enlarge(bounds,Vec3fa(max(r0,r1,r2,r3)));
        
#if 0
      /* perform subdivision */
      const Vec3fa diag = bounds.size();
      if (max(diag.x,diag.y) > ratio*diag.z && curves[i].dt > 0.1f) {
        Bezier1 left,right; curves[i].subdivide(left,right);
        curves[i] = left; curves.push_back(right);
        i--;
      }
#else
      const float Ab = area(bounds);
      const float Ac = len*2.0f*float(pi)*0.25f*(r0+r1+r2+r2);
      if (ratio*Ab > Ac && curves[i].dt() > 0.1f) {
        Bezier1 left,right; curves[i].subdivide(left,right);
        curves[i] = left; curves.push_back(right);
        i--;
      }
#endif
    }

    if (g_verbose >= 2) 
      std::cout << "  after  subdivision: " << 1E-6*float(curves.size()) << " M curves" << std::endl;
  }

  void BVH4HairBuilder::subdivide3()
  {
    if (g_verbose >= 2) 
      std::cout << std::endl << "  before subdivision: " << 1E-6*float(curves.size()) << " M curves" << std::endl;

    size_t N = curves.size();
    for (ssize_t i=0; i<N; i++)
    {
      Bezier1 a = curves[i];
      Bezier1 b0, b1;   a.subdivide(b0,b1);
      Bezier1 c00, c01; b0.subdivide(c00,c01);
      Bezier1 c10, c11; b1.subdivide(c10,c11);

      Bezier1 d000, d001; c00.subdivide(d000,d001);
      Bezier1 d010, d011; c01.subdivide(d010,d011);
      Bezier1 d100, d101; c10.subdivide(d100,d101);
      Bezier1 d110, d111; c11.subdivide(d110,d111);

      curves[i] = d000;
      curves.push_back(d001);
      curves.push_back(d010);
      curves.push_back(d011);
      curves.push_back(d100);
      curves.push_back(d101);
      curves.push_back(d110);
      curves.push_back(d111);
    }

    if (g_verbose >= 2) 
      std::cout << "  after  subdivision: " << 1E-6*float(curves.size()) << " M curves" << std::endl;
  }

  const BBox3fa BVH4HairBuilder::computeAlignedBounds(Bezier1* curves, size_t begin, size_t end)
  {
    float area = 0.0f;
    BBox3fa bounds = empty;
    for (size_t j=begin; j<end; j++) {
      const BBox3fa cbounds = curves[j].bounds();
      area += halfArea(cbounds);
      bounds.extend(cbounds);
    }
    bounds.upper.w = area;
    return bounds;
  }

  const BVH4Hair::NAABBox3fa BVH4HairBuilder::computeAlignedBounds(Bezier1* curves, size_t begin, size_t end, const LinearSpace3fa& space)
  {
    float area = 0.0f;
    BBox3fa bounds = empty;
    for (size_t j=begin; j<end; j++) {
      const BBox3fa cbounds = curves[j].bounds(space);
      area += halfArea(cbounds);
      bounds.extend(cbounds);
    }
    bounds.upper.w = area;
    return NAABBox3fa(space,bounds);
  }

  const BVH4Hair::NAABBox3fa BVH4HairBuilder::computeUnalignedBounds(Bezier1* curves, size_t begin, size_t end)
  {
    if (end-begin == 0)
      return NAABBox3fa(empty);

    float bestArea = inf;
    Vec3fa bestAxis = one;
    BBox3fa bestBounds = empty;

    for (size_t i=0; i<4; i++)
    {
      size_t k = begin + rand() % (end-begin);
      const Vec3fa axis = normalize(curves[k].p3-curves[k].p0);
      const LinearSpace3fa space = frame(axis).transposed();
      
      BBox3fa bounds = empty;
      float area = 0.0f;
      for (size_t j=begin; j<end; j++) {
        const BBox3fa cbounds = curves[j].bounds(space);
        area += halfArea(cbounds);
        bounds.extend(cbounds);
      }

      if (area <= bestArea) {
        bestBounds = bounds;
        bestAxis = axis;
        bestArea = area;
      }
    }
    bestBounds.upper.w = bestArea;

    return NAABBox3fa(frame(bestAxis).transposed(),bestBounds);
  }

  ////////////////////////////////////////////////////////////////////////////////////////////////////////////
  ////////////////////////////////////////////////////////////////////////////////////////////////////////////
  ////////////////////////////////////////////////////////////////////////////////////////////////////////////
  ////////////////////////////////////////////////////////////////////////////////////////////////////////////
  ////////////////////////////////////////////////////////////////////////////////////////////////////////////
  ////////////////////////////////////////////////////////////////////////////////////////////////////////////


  __forceinline BVH4HairBuilder::StrandSplit::StrandSplit (const NAABBox3fa& bounds0, const Vec3fa& axis0, const size_t num0,
                                                           const NAABBox3fa& bounds1, const Vec3fa& axis1, const size_t num1)
    : bounds0(bounds0), bounds1(bounds1), axis0(axis0), axis1(axis1), num0(num0), num1(num1) {}
  
  __forceinline const BVH4HairBuilder::StrandSplit BVH4HairBuilder::StrandSplit::find(Bezier1* curves, size_t begin, size_t end)
  {
    /* first try to split two hair strands */
    Vec3fa axis0 = normalize(curves[begin].p3-curves[begin].p0);
    float bestCos = 1.0f;
    size_t bestI = end-1;
    for (size_t i=begin; i<end; i++) {
      Vec3fa axisi = curves[i].p3-curves[i].p0;
      float leni = length(axisi);
      if (leni == 0.0f) continue;
      axisi /= leni;
      float cos = abs(dot(axisi,axis0));
      if (cos < bestCos) { bestCos = cos; bestI = i; }
    }
    Vec3fa axis1 = normalize(curves[bestI].p3-curves[bestI].p0);

    /* partition the two strands */
    ssize_t left = begin, right = end;
    while (left < right) {
      const Vec3fa axisi = normalize(curves[left].p3-curves[left].p0);
      const float cos0 = abs(dot(axisi,axis0));
      const float cos1 = abs(dot(axisi,axis1));
      if (cos0 > cos1) left++;
      else std::swap(curves[left],curves[--right]);
    }
    const size_t num0 = left-begin;
    const size_t num1 = end-left;
    if (num0 == 0 || num1 == 0)
      return StrandSplit(NAABBox3fa(one,inf),axis0,1,NAABBox3fa(one,inf),axis1,1);

    const NAABBox3fa naabb0 = computeUnalignedBounds(&curves[0],begin,left);
    const NAABBox3fa naabb1 = computeUnalignedBounds(&curves[0],left, end );
    return StrandSplit(naabb0,axis0,num0,naabb1,axis1,num1);
  }

  __forceinline size_t BVH4HairBuilder::StrandSplit::split(Bezier1* curves, size_t begin, size_t end) const
  {
    ssize_t left = begin, right = end;
    while (left < right) {
      const Vec3fa axisi = normalize(curves[left].p3-curves[left].p0);
      const float cos0 = abs(dot(axisi,axis0));
      const float cos1 = abs(dot(axisi,axis1));
      if (cos0 > cos1) left++;
      else std::swap(curves[left],curves[--right]);
    }
    assert(left-begin == num0);
    assert(end-left == num1);
    return left;
  }

  ////////////////////////////////////////////////////////////////////////////////////////////////////////////
  ////////////////////////////////////////////////////////////////////////////////////////////////////////////
  ////////////////////////////////////////////////////////////////////////////////////////////////////////////
  ////////////////////////////////////////////////////////////////////////////////////////////////////////////
  ////////////////////////////////////////////////////////////////////////////////////////////////////////////
  ////////////////////////////////////////////////////////////////////////////////////////////////////////////

  __forceinline BVH4HairBuilder::ObjectSplit BVH4HairBuilder::ObjectSplit::find(Bezier1* curves, size_t begin, size_t end, const LinearSpace3fa& space)
  {
    /* calculate geometry and centroid bounds */
    BBox3fa centBounds = empty;
    BBox3fa geomBounds = empty;
    for (size_t i=begin; i<end; i++) {
      geomBounds.extend(curves[i].bounds(space));
      centBounds.extend(curves[i].center(space));
    }

    /* calculate binning function */
    const ssef ofs  = (ssef) centBounds.lower;
    const ssef diag = (ssef) centBounds.size();
    const ssef scale = select(diag != 0.0f,rcp(diag) * ssef(BINS * 0.99f),ssef(0.0f));

    /* initialize bins */
    BBox3fa bounds[BINS][4];
    ssei    counts[BINS];
    for (size_t i=0; i<BINS; i++) {
      bounds[i][0] = bounds[i][1] = bounds[i][2] = bounds[i][3] = empty;
      counts[i] = 0;
    }
 
    /* perform binning of curves */
    for (size_t i=begin; i<end; i++)
    {
      const BBox3fa cbounds = curves[i].bounds(space);
      const Vec3fa  center  = curves[i].center(space);
      //const ssei bin = clamp(floori((ssef(center) - ofs)*scale),ssei(0),ssei(BINS-1));
      const ssei bin = floori((ssef(center) - ofs)*scale);
      assert(bin[0] >=0 && bin[0] < BINS);
      assert(bin[1] >=0 && bin[1] < BINS);
      assert(bin[2] >=0 && bin[2] < BINS);
      const int b0 = bin[0]; counts[b0][0]++; bounds[b0][0].extend(cbounds);
      const int b1 = bin[1]; counts[b1][1]++; bounds[b1][1].extend(cbounds);
      const int b2 = bin[2]; counts[b2][2]++; bounds[b2][2].extend(cbounds);
    }
    
    /* sweep from right to left and compute parallel prefix of merged bounds */
    ssef rAreas[BINS];
    ssei rCounts[BINS];
    ssei count = 0; BBox3fa bx = empty; BBox3fa by = empty; BBox3fa bz = empty;
    for (size_t i=BINS-1; i>0; i--)
    {
      count += counts[i];
      rCounts[i] = count;
      bx.extend(bounds[i][0]); rAreas[i][0] = area(bx);
      by.extend(bounds[i][1]); rAreas[i][1] = area(by);
      bz.extend(bounds[i][2]); rAreas[i][2] = area(bz);
    }
    
    /* sweep from left to right and compute SAH */
    ssei ii = 1; ssef bestSAH = pos_inf; ssei bestPos = 0; ssei bestLeft = 0;
    count = 0; bx = empty; by = empty; bz = empty;
    for (size_t i=1; i<BINS; i++, ii+=1)
    {
      count += counts[i-1];
      bx.extend(bounds[i-1][0]); float Ax = area(bx);
      by.extend(bounds[i-1][1]); float Ay = area(by);
      bz.extend(bounds[i-1][2]); float Az = area(bz);
      const ssef lArea = ssef(Ax,Ay,Az,Az);
      const ssef rArea = rAreas[i];
#if BVH4HAIR_WIDTH == 8
      const ssei lCount = (count     +ssei(7)) >> 3;
      const ssei rCount = (rCounts[i]+ssei(7)) >> 3;
#else
      const ssei lCount = (count     +ssei(3)) >> 2;
      const ssei rCount = (rCounts[i]+ssei(3)) >> 2;
      //const ssei lCount = count;
      //const ssei rCount = rCounts[i];
#endif
      const ssef sah = lArea*ssef(lCount) + rArea*ssef(rCount);
      bestPos = select(sah < bestSAH,ii ,bestPos);
      bestLeft= select(sah < bestSAH,count,bestLeft);
      bestSAH = select(sah < bestSAH,sah,bestSAH);
    }
    
    /* find best dimension */
    ObjectSplit split;
    split.space = space;
    split.ofs = ofs;
    split.scale = scale;

    for (size_t dim=0; dim<3; dim++) 
    {
      /* ignore zero sized dimensions */
      if (unlikely(scale[dim] == 0.0f)) 
        continue;
      
      /* test if this is a better dimension */
      if (bestSAH[dim] < split.cost && bestPos[dim] != 0) {
        split.dim = dim;
        split.pos = bestPos[dim];
        split.cost = bestSAH[dim];
        split.num0 = bestLeft[dim];
        split.num1 = end-begin-split.num0;
      }
    }
    return split;
  }

  const BVH4HairBuilder::ObjectSplit BVH4HairBuilder::ObjectSplit::alignedBounds(Bezier1* curves, size_t begin, size_t end)
  {
    if (dim == -1) {
      num0 = num1 = 1;
      bounds0 = bounds1 = BBox3fa(inf);
      return *this;
    }

    const size_t center = split(&curves[0],begin,end);
    bounds0 = computeAlignedBounds(&curves[0],begin,center);
    bounds1 = computeAlignedBounds(&curves[0],center,end);
    return *this;
  }
  
  const BVH4HairBuilder::ObjectSplit  BVH4HairBuilder::ObjectSplit::unalignedBounds(Bezier1* curves, size_t begin, size_t end)
  {
    if (dim == -1) {
      num0 = num1 = 1;
      bounds0 = bounds1 = BBox3fa(inf);
      return *this;
    }

    const size_t center = split(&curves[0],begin,end);
    bounds0 = computeUnalignedBounds(&curves[0],begin,center);
    bounds1 = computeUnalignedBounds(&curves[0],center,end);
    return *this;
  }
  
  __forceinline size_t BVH4HairBuilder::ObjectSplit::split(Bezier1* curves, size_t begin, size_t end) const
  {
    ssize_t left = begin, right = end;
    while (left < right) {
      const Vec3fa center = curves[left].center(space);
      //const ssei bin = clamp(floori((ssef(center) - ofs)*scale),ssei(0),ssei(BINS-1));
      const ssei bin = floori((ssef(center)-ofs)*scale);
      if (bin[dim] < pos) left++;
      else std::swap(curves[left],curves[--right]);
    }
    assert(left-begin == num0);
    assert(end-left == num1);
    return left;
  }

  ////////////////////////////////////////////////////////////////////////////////////////////////////////////
  ////////////////////////////////////////////////////////////////////////////////////////////////////////////
  ////////////////////////////////////////////////////////////////////////////////////////////////////////////
  ////////////////////////////////////////////////////////////////////////////////////////////////////////////
  ////////////////////////////////////////////////////////////////////////////////////////////////////////////
  ////////////////////////////////////////////////////////////////////////////////////////////////////////////

  __forceinline BVH4HairBuilder::SubdivObjectSplit BVH4HairBuilder::SubdivObjectSplit::find(Bezier1* curves, size_t begin, size_t end, const LinearSpace3fa& space)
  {
    /* calculate geometry and centroid bounds */
    BBox3fa centBounds = empty;
    BBox3fa geomBounds = empty;
    for (size_t i=begin; i<end; i++) 
    {
      Bezier1 left,right; 
      curves[i].subdivide(left,right);
      geomBounds.extend(left .bounds(space));
      centBounds.extend(left .center(space));
      geomBounds.extend(right.bounds(space));
      centBounds.extend(right.center(space));
    }

    /* calculate binning function */ 
    const ssef ofs  = (ssef) centBounds.lower;
    const ssef diag = (ssef) centBounds.size();
    const ssef scale = select(diag != 0.0f,rcp(diag) * ssef(BINS * 0.99f),ssef(0.0f));

    /* initialize bins */
    BBox3fa bounds[BINS][4];
    float   areas [BINS][4];
    ssei    counts[BINS];
    for (size_t i=0; i<BINS; i++) {
      bounds[i][0] = bounds[i][1] = bounds[i][2] = bounds[i][3] = empty;
      areas [i][0] = areas [i][1] = areas [i][2] = areas [i][3] = 0.0f;
      counts[i] = 0;
    }
 
    /* perform binning of curves */
    for (size_t i=begin; i<end; i++)
    {
      Bezier1 left,right; 
      curves[i].subdivide(left,right);
      {
        const BBox3fa lbounds = left .bounds(space);
        const Vec3fa  lcenter = left .center(space);
        //const ssei bin = clamp(floori((ssef(lcenter) - ofs)*scale),ssei(0),ssei(BINS-1));
        const ssei bin = floori((ssef(lcenter)-ofs)*scale);
        assert(bin[0] >=0 && bin[0] < BINS);
        assert(bin[1] >=0 && bin[1] < BINS);
        assert(bin[2] >=0 && bin[2] < BINS);
        const int b0 = bin[0]; counts[b0][0]++; bounds[b0][0].extend(lbounds); areas[b0][0] += halfArea(lbounds);
        const int b1 = bin[1]; counts[b1][1]++; bounds[b1][1].extend(lbounds); areas[b1][1] += halfArea(lbounds);
        const int b2 = bin[2]; counts[b2][2]++; bounds[b2][2].extend(lbounds); areas[b2][2] += halfArea(lbounds);
      }
      {
        const BBox3fa rbounds = right.bounds(space);
        const Vec3fa  rcenter = right.center(space);
        //const ssei bin = clamp(floori((ssef(rcenter) - ofs)*scale),ssei(0),ssei(BINS-1));
        const ssei bin = floori((ssef(rcenter)-ofs)*scale);
        assert(bin[0] >=0 && bin[0] < BINS);
        assert(bin[1] >=0 && bin[1] < BINS);
        assert(bin[2] >=0 && bin[2] < BINS);
        const int b0 = bin[0]; counts[b0][0]++; bounds[b0][0].extend(rbounds); areas[b0][0] += halfArea(rbounds);
        const int b1 = bin[1]; counts[b1][1]++; bounds[b1][1].extend(rbounds); areas[b1][1] += halfArea(rbounds);
        const int b2 = bin[2]; counts[b2][2]++; bounds[b2][2].extend(rbounds); areas[b2][2] += halfArea(rbounds);
      }
    }
    
    /* sweep from right to left and compute parallel prefix of merged bounds */
    ssef rAreas[BINS];
    ssei rCounts[BINS];
    ssei count = 0; BBox3fa bx = empty; BBox3fa by = empty; BBox3fa bz = empty;
    for (size_t i=BINS-1; i>0; i--)
    {
      count += counts[i];
      rCounts[i] = count;
      bx.extend(bounds[i][0]); rAreas[i][0] = area(bx);
      by.extend(bounds[i][1]); rAreas[i][1] = area(by);
      bz.extend(bounds[i][2]); rAreas[i][2] = area(bz);
    }
    
    /* sweep from left to right and compute SAH */
    ssei ii = 1; ssef bestSAH = pos_inf; ssei bestPos = 0; ssei bestLeft = 0;
    count = 0; bx = empty; by = empty; bz = empty;
    for (size_t i=1; i<BINS; i++, ii+=1)
    {
      count += counts[i-1];
      bx.extend(bounds[i-1][0]); float Ax = area(bx);
      by.extend(bounds[i-1][1]); float Ay = area(by);
      bz.extend(bounds[i-1][2]); float Az = area(bz);
      const ssef lArea = ssef(Ax,Ay,Az,Az);
      const ssef rArea = rAreas[i];
#if BVH4HAIR_WIDTH == 8
      const ssei lCount = (count     +ssei(7)) >> 3;
      const ssei rCount = (rCounts[i]+ssei(7)) >> 3;
#else
      const ssei lCount = (count     +ssei(3)) >> 2;
      const ssei rCount = (rCounts[i]+ssei(3)) >> 2;
      //const ssei lCount = count;
      //const ssei rCount = rCounts[i];
#endif
      const ssef sah = lArea*ssef(lCount) + rArea*ssef(rCount);
      bestPos = select(sah < bestSAH,ii ,bestPos);
      bestLeft= select(sah < bestSAH,count,bestLeft);
      bestSAH = select(sah < bestSAH,sah,bestSAH);
    }
    
    /* find best dimension */
    SubdivObjectSplit split;
    split.space = space;
    split.ofs = ofs;
    split.scale = scale;

    for (size_t dim=0; dim<3; dim++) 
    {
      /* ignore zero sized dimensions */
      if (unlikely(scale[dim] == 0.0f)) 
        continue;
      
      /* test if this is a better dimension */
      if (bestSAH[dim] < split.cost && bestPos[dim] != 0) {
        split.dim = dim;
        split.pos = bestPos[dim];
        split.cost = bestSAH[dim];
        split.num0 = bestLeft[dim];
        split.num1 = 2*(end-begin)-split.num0;
      }
    }

    /* compute bounds of left and right side */
    if (split.dim == -1) {
      split.num0 = split.num1 = 1; // avoids NANs in SAH calculation
      split.bounds0 = split.bounds1 = BBox3fa(inf);
    }
    else 
    {
      BBox3fa lbounds = empty, rbounds = empty;
      float   larea   = 0.0f,  rarea   = 0.0f;
      for (size_t i=0; i<split.pos;    i++) { lbounds.extend(bounds[i][split.dim]); larea += areas[i][split.dim]; }
      for (size_t i=split.pos; i<BINS; i++) { rbounds.extend(bounds[i][split.dim]); rarea += areas[i][split.dim]; }
      lbounds.upper.w = larea;
      rbounds.upper.w = rarea;
      split.bounds0 = NAABBox3fa(space,lbounds);
      split.bounds1 = NAABBox3fa(space,rbounds);
    }
    return split;
  }

  __forceinline size_t BVH4HairBuilder::SubdivObjectSplit::split(Bezier1* curves, size_t begin, size_t& end) const
  {
    /*! first subdivide all curves */
    ssize_t left = begin, right = end;
    for (size_t i=left; i<right; i++)
    {
      Bezier1 left,right; 
      curves[i].subdivide(left,right);
      curves[i] = left;
      curves[end++] = right;
    }

    /* now split into left and right */
    left = begin; right = end;
    while (left < right) 
    {
      const Vec3fa center = curves[left].center(space);
      //const ssei bin = clamp(floori((ssef(center)-ofs)*scale),ssei(0),ssei(BINS-1));
      const ssei bin = floori((ssef(center)-ofs)*scale);
      if (bin[dim] < pos) left++;
      else std::swap(curves[left],curves[--right]);
    }
    assert(left-begin == num0);
    assert(end-left == num1);
    return left;
  }

  ////////////////////////////////////////////////////////////////////////////////////////////////////////////
  ////////////////////////////////////////////////////////////////////////////////////////////////////////////
  ////////////////////////////////////////////////////////////////////////////////////////////////////////////
  ////////////////////////////////////////////////////////////////////////////////////////////////////////////
  ////////////////////////////////////////////////////////////////////////////////////////////////////////////
  ////////////////////////////////////////////////////////////////////////////////////////////////////////////

  const BVH4HairBuilder::SpatialCenterSplit BVH4HairBuilder::SpatialCenterSplit::find(Bezier1* curves, size_t begin, size_t end, const LinearSpace3fa& space)
  {
    /* calculate geometry bounds */
    BBox3fa geomBounds = empty;
    for (size_t i=begin; i<end; i++) 
      geomBounds.extend(curves[i].bounds(space));
    Vec3fa cent = center(geomBounds);
    
    /* test spatial split in each dimension */
    float bestSAH = inf;
    int   bestDim = -1;
    float bestPos = 0.0f;
    BBox3fa bestLeftBounds  = empty;
    BBox3fa bestRightBounds = empty;
    int bestLeftNum = 0;
    int bestRightNum = 0;

    for (int dim=0; dim<3; dim++)
    {
      /* calculate splitting plane */
      const Vec3fa plane(space.vx[dim],space.vy[dim],space.vz[dim],-cent[dim]);

      /* sort each curve to left, right, or left and right */
      size_t lnum = 0, rnum = 0;
      BBox3fa lbounds = empty, rbounds = empty;
      float larea = 0.0f, rarea = 0.0f;
      for (size_t i=begin; i<end; i++) 
      {
        const float p0p = dot(curves[i].p0,plane)+plane.w;
        const float p3p = dot(curves[i].p3,plane)+plane.w;

        /* sort to the left side */
        if (p0p <= 0.0f && p3p <= 0.0f) 
        {
          const BBox3fa bounds = curves[i].bounds(space);
          lbounds.extend(bounds);
          larea += halfArea(bounds);
          lnum++;
          continue;
        }

        /* sort to the right side */
        if (p0p >= 0.0f && p3p >= 0.0f) 
        {
          const BBox3fa bounds = curves[i].bounds(space);
          rbounds.extend(bounds);
          rarea += halfArea(bounds);
          rnum++;
          continue;
        }

        /* split and sort to left and right */
        Bezier1 left,right;
        if (curves[i].split(plane,left,right)) {
          const BBox3fa lb = left .bounds(space);
          const BBox3fa rb = right.bounds(space);
          lbounds.extend(lb); larea += halfArea(lb); lnum++;
          rbounds.extend(rb); rarea += halfArea(rb); rnum++;
          continue;
        }

        /* fallback in case we could not split the curve */
        const BBox3fa bounds = curves[i].bounds(space);
        lbounds.extend(bounds);
        larea += halfArea(bounds);
        lnum++;
      }
      lbounds.upper.w = larea;
      rbounds.upper.w = rarea;

      if (lnum == 0 || rnum == 0) 
        continue;

      /* check if current dimension gives best SAH */
      const float sah = halfArea(lbounds)*float(lnum) + halfArea(rbounds)*float(rnum);
      if (sah < bestSAH) {
        bestSAH = sah;
        bestDim = dim;
        bestPos = cent[dim];
        bestLeftBounds = lbounds;
        bestRightBounds = rbounds;
        bestLeftNum = lnum;
        bestRightNum = rnum;
      }
    }

    return SpatialCenterSplit(space,bestPos,bestDim,
                              NAABBox3fa(space,bestLeftBounds),bestLeftNum,
                              NAABBox3fa(space,bestRightBounds),bestRightNum);
  }
      
  size_t BVH4HairBuilder::SpatialCenterSplit::split(Bezier1* curves, size_t begin, size_t& end) const
  {
    /* calculate splitting plane */
    const Vec3fa plane(space.vx[dim],space.vy[dim],space.vz[dim],-pos);
    
    /* sort each curve to left, right, or left and right */
    size_t end2 = end;
    while (begin < end)
    {
      const float p0p = dot(curves[begin].p0,plane)+plane.w;
      const float p3p = dot(curves[begin].p3,plane)+plane.w;

      /* sort to the left side */
      if (p0p <= 0.0f && p3p <= 0.0f) {
        begin++;
        continue;
      }

      /* sort to the right side */
      if (p0p >= 0.0f && p3p >= 0.0f) {
        std::swap(curves[begin],curves[--end]);
        continue;
      }

      /* split and sort to left and right */
      Bezier1 left,right;
      curves[begin].split(plane,left,right);
      curves[begin++] = left;
      curves[end2++] = right;
    }
    end = end2;
    return begin;
  }

  __forceinline BVH4HairBuilder::FallBackSplit BVH4HairBuilder::FallBackSplit::find(Bezier1* curves, size_t begin, size_t end)
  {
    const size_t center = (begin+end)/2;
    const BBox3fa bounds0 = computeAlignedBounds(&curves[0],begin,center);
    const BBox3fa bounds1 = computeAlignedBounds(&curves[0],center,end);
    return FallBackSplit(center,bounds0,bounds1);
  }

  ////////////////////////////////////////////////////////////////////////////////////////////////////////////
  ////////////////////////////////////////////////////////////////////////////////////////////////////////////
  ////////////////////////////////////////////////////////////////////////////////////////////////////////////
  ////////////////////////////////////////////////////////////////////////////////////////////////////////////
  ////////////////////////////////////////////////////////////////////////////////////////////////////////////
  ////////////////////////////////////////////////////////////////////////////////////////////////////////////

  BVH4Hair::NodeRef BVH4HairBuilder::leaf(size_t threadIndex, size_t depth, size_t begin, size_t end, const NAABBox3fa& bounds)
  {
    size_t N = end-begin;
    if (N > (size_t)BVH4Hair::maxLeafBlocks) {
      std::cout << "WARNING: Loosing " << N-BVH4Hair::maxLeafBlocks << " primitives during build!" << std::endl;
      N = (size_t)BVH4Hair::maxLeafBlocks;
    }
    numGeneratedPrims+=N; if (numGeneratedPrims > 10000) { std::cout << "." << std::flush; numGeneratedPrims = 0; }
    //assert(N <= (size_t)BVH4Hair::maxLeafBlocks);
    Bezier1* leaf = (Bezier1*) bvh->allocPrimitiveBlocks(threadIndex,N);
    for (size_t i=0; i<N; i++) leaf[i] = curves[begin+i];
    return bvh->encodeLeaf((char*)leaf,N);
  }

  size_t BVH4HairBuilder::split(size_t depth, size_t begin, size_t& end, const NAABBox3fa& bounds, NAABBox3fa& lbounds, NAABBox3fa& rbounds, bool& isAligned)
  {
    /* variable to track the SAH of the best splitting approach */
    float bestSAH = inf;
    const int travCostAligned = isAligned ? BVH4Hair::travCostAligned : BVH4Hair::travCostUnaligned;
    
    /* perform standard binning in aligned space */
#if ENABLE_ALIGNED_SPLITS && ENABLE_OBJECT_SPLITS
    const ObjectSplit alignedObjectSplit = ObjectSplit::find(&curves[0],begin,end).alignedBounds(&curves[0],begin,end);
    const float alignedObjectSAH = travCostAligned*halfArea(bounds.bounds) + alignedObjectSplit.modifiedSAH();
    //const float alignedObjectSAH = travCostAligned*halfArea(bounds.bounds) + alignedObjectSplit.standardSAH();
    bestSAH = min(bestSAH,alignedObjectSAH);
#endif

    /* perform spatial split in aligned space */
#if ENABLE_ALIGNED_SPLITS && ENABLE_SPATIAL_SPLITS
    const SpatialCenterSplit alignedSpatialSplit = SpatialCenterSplit::find(&curves[0],begin,end);
    const float alignedSpatialSAH = travCostAligned*halfArea(bounds.bounds) + alignedSpatialSplit.modifiedSAH();
    //const float alignedSpatialSAH = travCostAligned*halfArea(bounds.bounds) + alignedSpatialSplit.standardSAH();
    bestSAH = min(bestSAH,alignedSpatialSAH);
#endif

    /* perform binning with subdivision in aligned space */
#if ENABLE_ALIGNED_SPLITS && ENABLE_SUBDIV_SPLITS
    const SubdivObjectSplit alignedSubdivObjectSplit = SubdivObjectSplit::find(&curves[0],begin,end);
    const float alignedSubdivObjectSAH = travCostAligned*halfArea(bounds.bounds) + alignedSubdivObjectSplit.modifiedSAH();
    //const float alignedSubdivObjectSAH = travCostAligned*halfArea(bounds.bounds) + alignedSubdivObjectSplit.standardSAH();
    bestSAH = min(bestSAH,alignedSubdivObjectSAH);
#endif

    /* perform standard binning in unaligned space */
#if ENABLE_UNALIGNED_SPLITS && ENABLE_OBJECT_SPLITS
    const ObjectSplit unalignedObjectSplit = ObjectSplit::find(&curves[0],begin,end,bounds.space).unalignedBounds(&curves[0],begin,end);
    const float unalignedObjectSAH = BVH4Hair::travCostUnaligned*halfArea(bounds.bounds) + unalignedObjectSplit.modifiedSAH();
    //const float unalignedObjectSAH = BVH4Hair::travCostUnaligned*halfArea(bounds.bounds) + unalignedObjectSplit.standardSAH();
    bestSAH = min(bestSAH,unalignedObjectSAH);
#endif

    /* perform spatial split in unaligned space */
#if ENABLE_UNALIGNED_SPLITS && ENABLE_SPATIAL_SPLITS
    const SpatialCenterSplit unalignedSpatialSplit = SpatialCenterSplit::find(&curves[0],begin,end,bounds.space);
    const float unalignedSpatialSAH = BVH4Hair::travCostUnaligned*halfArea(bounds.bounds) + unalignedSpatialSplit.modifiedSAH();
    //const float unalignedSpatialSAH = BVH4Hair::travCostUnaligned*halfArea(bounds.bounds) + unalignedSpatialSplit.standardSAH();
    bestSAH = min(bestSAH,unalignedSpatialSAH);
#endif

    /* perform binning with subdivision in unaligned space */
#if ENABLE_UNALIGNED_SPLITS && ENABLE_SUBDIV_SPLITS
    const SubdivObjectSplit unalignedSubdivObjectSplit = SubdivObjectSplit::find(&curves[0],begin,end,bounds.space);
    const float unalignedSubdivObjectSAH = BVH4Hair::travCostUnaligned*halfArea(bounds.bounds) + unalignedSubdivObjectSplit.modifiedSAH();
    //const float unalignedSubdivObjectSAH = BVH4Hair::travCostUnaligned*halfArea(bounds.bounds) + unalignedSubdivObjectSplit.standardSAH();
    bestSAH = min(bestSAH,unalignedSubdivObjectSAH);
#endif

    /* perform splitting into two strands */
#if ENABLE_UNALIGNED_SPLITS && ENABLE_STRAND_SPLITS
    const StrandSplit strandSplit = StrandSplit::find(&curves[0],begin,end);
    const float strandSAH = BVH4Hair::travCostUnaligned*halfArea(bounds.bounds) + strandSplit.modifiedSAH();
    //const float strandSAH = BVH4Hair::travCostUnaligned*halfArea(bounds.bounds) + strandSplit.standardSAH();
    bestSAH = min(bestSAH,strandSAH);
#endif

    /* perform fallback split */
    if (bestSAH == float(inf)) {
      numFallbackSplits++;
      const FallBackSplit fallbackSplit = FallBackSplit::find(&curves[0],begin,end);
      assert((fallbackSplit.center-begin > 0) && (end-fallbackSplit.center) > 0);
      lbounds = fallbackSplit.bounds0;
      rbounds = fallbackSplit.bounds1;
      return fallbackSplit.center;
    }

    /* perform aligned object split */
#if ENABLE_ALIGNED_SPLITS && ENABLE_OBJECT_SPLITS
    else if (bestSAH == alignedObjectSAH) {
      numAlignedObjectSplits++;
      const size_t center = alignedObjectSplit.split(&curves[0],begin,end);
      assert((center-begin > 0) && (end-center) > 0);
      lbounds = alignedObjectSplit.bounds0;
      rbounds = alignedObjectSplit.bounds1;
      return center;
    }
#endif

    /* perform aligned spatial split */
#if ENABLE_ALIGNED_SPLITS && ENABLE_SPATIAL_SPLITS
    else if (bestSAH == alignedSpatialSAH) {
      numAlignedSpatialSplits++;
      const size_t center = alignedSpatialSplit.split(&curves[0],begin,end);
      assert((center-begin > 0) && (end-center) > 0);
      lbounds = alignedSpatialSplit.bounds0;
      rbounds = alignedSpatialSplit.bounds1;
      return center;
    }
#endif

    /* perform aligned object split with subdivision */
#if ENABLE_ALIGNED_SPLITS && ENABLE_SUBDIV_SPLITS
    else if (bestSAH == alignedSubdivObjectSAH) {
      numAlignedSubdivObjectSplits++;
      const size_t center = alignedSubdivObjectSplit.split(&curves[0],begin,end);
      assert((center-begin > 0) && (end-center) > 0);
      lbounds = alignedSubdivObjectSplit.bounds0;
      rbounds = alignedSubdivObjectSplit.bounds1;
      return center;
    }
#endif

    /* perform unaligned object split */
#if ENABLE_UNALIGNED_SPLITS && ENABLE_OBJECT_SPLITS
    else if (bestSAH == unalignedObjectSAH) {
      numUnalignedObjectSplits++;
      const size_t center = unalignedObjectSplit.split(&curves[0],begin,end);
      assert((center-begin > 0) && (end-center) > 0);
      lbounds = unalignedObjectSplit.bounds0;
      rbounds = unalignedObjectSplit.bounds1;
      isAligned = false;
      return center;
    }
#endif

    /* perform unaligned spatial split */
#if ENABLE_UNALIGNED_SPLITS && ENABLE_SPATIAL_SPLITS
    else if (bestSAH == unalignedSpatialSAH) {
      numUnalignedSpatialSplits++;
      const size_t center = unalignedSpatialSplit.split(&curves[0],begin,end);
      assert((center-begin > 0) && (end-center) > 0);
      lbounds = unalignedSpatialSplit.bounds0;
      rbounds = unalignedSpatialSplit.bounds1;
      return center;
    }
#endif

    /* perform unaligned object split with subdivision */
#if ENABLE_UNALIGNED_SPLITS && ENABLE_SUBDIV_SPLITS
    else if (bestSAH == unalignedSubdivObjectSAH) {
      numUnalignedSubdivObjectSplits++;
      const size_t center = unalignedSubdivObjectSplit.split(&curves[0],begin,end);
      assert((center-begin > 0) && (end-center) > 0);
      lbounds = unalignedSubdivObjectSplit.bounds0;
      rbounds = unalignedSubdivObjectSplit.bounds1;
      return center;
    }
#endif

    /* perform strand split */
#if ENABLE_UNALIGNED_SPLITS && ENABLE_STRAND_SPLITS
    else if (bestSAH == strandSAH) {
      numStrandSplits++;
      const size_t center = strandSplit.split(&curves[0],begin,end);
      assert((center-begin > 0) && (end-center) > 0);
      lbounds = strandSplit.bounds0;
      rbounds = strandSplit.bounds1;
      isAligned = false;
      return center;
    }
#endif
 
    else {
      throw std::runtime_error("bvh4hair_builder: internal error");
    }
  }

  BVH4Hair::NodeRef BVH4HairBuilder::recurse(size_t threadIndex, size_t depth, size_t begin, size_t end, const NAABBox3fa& bounds)
  {
    /* create enforced leaf */
    const size_t N = end-begin;
    if (N <= minLeafSize || depth > BVH4Hair::maxBuildDepth)
      return leaf(threadIndex,depth,begin,end,bounds);

    /*! initialize child list */
    bool isAligned = true;
    size_t cbegin [BVH4Hair::N];
    size_t cend   [BVH4Hair::N];
    NAABBox3fa cbounds[BVH4Hair::N];
    cbegin[0] = begin;
    cend  [0] = end;
    cbounds[0] = bounds;
    size_t numChildren = 1;
    
    /*! split until node is full or SAH tells us to stop */
    do {
      
      /*! find best child to split */
      float bestArea = neg_inf; 
      ssize_t bestChild = -1;
      for (size_t i=0; i<numChildren; i++) 
      {
        size_t N = cend[i]-cbegin[i];
        float A = halfArea(cbounds[i].bounds);
        if (N <= minLeafSize) continue;  
        if (A > bestArea) { bestChild = i; bestArea = A; }
      }
      if (bestChild == -1) break;

      /*! move selected child to the right, required for spatial splits and subdiv splits !!! */
#if ENABLE_SPATIAL_SPLITS || ENABLE_SUBDIV_SPLITS
      for (size_t c=bestChild+1; c<numChildren; c++)
      {
        ssize_t j,k;
        size_t c0 = c-1, c1 = c;
        std::swap(cbounds[c0],cbounds[c1]);
        size_t s0 = cend[c0]-cbegin[c0];
        size_t s1 = cend[c1]-cbegin[c1];
        size_t num = min(s0,s1);
        for (j=cbegin[c0], k=cend[c1]; j<cbegin[c0]+num; j++, k--) 
          std::swap(curves[j],curves[k-1]);
        if (s0<s1) { cend[c0] = k; cbegin[c1] = k; }
        else       { cend[c0] = j; cbegin[c1] = j; }
      }
      bestChild = numChildren-1;
#endif
      
      /*! split selected child */
      NAABBox3fa lbounds, rbounds;
      size_t center = split(depth,cbegin[bestChild],cend[bestChild],cbounds[bestChild],lbounds,rbounds,isAligned);
      cbounds[numChildren] = rbounds; cbegin[numChildren] = center; cend[numChildren] = cend[bestChild];
      cbounds[bestChild  ] = lbounds;                               cend[bestChild  ] = center; 
      numChildren++;
      
    } while (numChildren < BVH4Hair::N);
    
    /* create aligned node */
    if (isAligned) {
      AlignedNode* node = bvh->allocAlignedNode(threadIndex);
      for (ssize_t i=numChildren-1; i>=0; i--)
        node->set(i,cbounds[i].bounds,recurse(threadIndex,depth+1,cbegin[i],cend[i],cbounds[i]));
      return bvh->encodeNode(node);
    }
    
    /* create unaligned node */
    else {
      UnalignedNode* node = bvh->allocUnalignedNode(threadIndex);
      for (ssize_t i=numChildren-1; i>=0; i--)
        node->set(i,cbounds[i],recurse(threadIndex,depth+1,cbegin[i],cend[i],cbounds[i]));
      return bvh->encodeNode(node);
    }
  }

  Builder* BVH4HairBuilder_ (BVH4Hair* accel, Scene* scene) {
    return new BVH4HairBuilder(accel,scene);
  }
}
