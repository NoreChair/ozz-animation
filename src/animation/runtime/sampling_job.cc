//----------------------------------------------------------------------------//
//                                                                            //
// ozz-animation is hosted at http://github.com/guillaumeblanc/ozz-animation  //
// and distributed under the MIT License (MIT).                               //
//                                                                            //
// Copyright (c) 2019 Guillaume Blanc                                         //
//                                                                            //
// Permission is hereby granted, free of charge, to any person obtaining a    //
// copy of this software and associated documentation files (the "Software"), //
// to deal in the Software without restriction, including without limitation  //
// the rights to use, copy, modify, merge, publish, distribute, sublicense,   //
// and/or sell copies of the Software, and to permit persons to whom the      //
// Software is furnished to do so, subject to the following conditions:       //
//                                                                            //
// The above copyright notice and this permission notice shall be included in //
// all copies or substantial portions of the Software.                        //
//                                                                            //
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR //
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,   //
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL    //
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER //
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING    //
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER        //
// DEALINGS IN THE SOFTWARE.                                                  //
//                                                                            //
//----------------------------------------------------------------------------//

#include "ozz/animation/runtime/sampling_job.h"

#include <cassert>

#include "ozz/animation/runtime/animation.h"
#include "ozz/base/maths/math_constant.h"
#include "ozz/base/maths/math_ex.h"
#include "ozz/base/maths/soa_transform.h"
#include "ozz/base/memory/allocator.h"

// Internal include file
#define OZZ_INCLUDE_PRIVATE_HEADER  // Allows to include private headers.
#include "animation/runtime/animation_keyframe.h"

namespace ozz {
namespace animation {

namespace internal {
struct InterpSoaFloat3 {
  math::SimdFloat4 ratio[2];
  math::SoaFloat3 value[2];
};
struct InterpSoaQuaternion {
  math::SimdFloat4 ratio[2];
  math::SoaQuaternion value[2];
};
}  // namespace internal

bool SamplingJob::Validate() const {
  // Don't need any early out, as jobs are valid in most of the performance
  // critical cases.
  // Tests are written in multiple lines in order to avoid branches.
  bool valid = true;

  // Test for nullptr pointers.
  if (!animation || !cache) {
    return false;
  }
  valid &= output.begin != nullptr;

  // Tests output range, implicitly tests output.end != nullptr.
  const ptrdiff_t num_soa_tracks = animation->num_soa_tracks();
  valid &= output.end - output.begin >= num_soa_tracks;

  // Tests cache size.
  valid &= cache->max_soa_tracks() >= num_soa_tracks;

  return valid;
}

namespace {

class Typer {
 public:
  Typer(const Range<const uint8_t>& _types, TrackType _type)
      : types_(_types), type_(static_cast<uint8_t>(_type)), current_(0) {}
  size_t operator++() {
    // Out of bound is asserted by types_ Range.
    for (uint8_t type = 0xff; type != type_; ++current_) {
      const size_t byte_offset = current_ / 4;
      const size_t bit_offset = (current_ - byte_offset * 4) * 2;
      type = (types_[byte_offset] >> bit_offset) & 0x3;
    }
    return current_ - 1;
  }

 private:
  Range<const uint8_t> types_;
  uint8_t type_;
  size_t current_;
};

// Loops through the sorted key frames and update cache structure.
template <typename _Key>
void UpdateCacheCursor(float _ratio, const ozz::Range<const _Key>& _keys,
                       int* _cursor, int* _cache, unsigned char* _outdated,
                       int* _count) {
  if (_keys.count() == 0) {
    *_count = 0;
    return;
  }
  assert(_keys.count() >= 2 &&
         "There must always be a first and a last key minimum");
  //  assert(_keys.begin + num_tracks * 2 <= _keys.end);

  const _Key* cursor = &_keys.begin[*_cursor];
  if (!*_cursor) {
    // This first loop computes animation track count. This forces to load
    // keyframes to cache, but this keyframes will be needed right after anyway
    // for interpolation.
    int num_animated_tracks = 1;
    for (uint16_t prev = _keys[0].track, next = _keys[1].track; prev < next;
         prev = next, next = _keys[++num_animated_tracks].track) {
    }
    const int num_animated_soa_tracks = (num_animated_tracks + 3) / 4;

    // Updates cache animtated track count;
    *_count = num_animated_soa_tracks;

    // Initializes interpolated entries with the first 2 sets of keyframes.
    // The sorting algorithm ensures that the first 2 keyframes of a track
    // are consecutive.
    for (int i = 0; i < num_animated_soa_tracks; ++i) {
      const int in_index0 = i * 4;                            // * soa size
      const int in_index1 = in_index0 + num_animated_tracks;  // 2nd row.
      const int out_index = i * 4 * 2;
      _cache[out_index + 0] = in_index0 + 0;
      _cache[out_index + 1] = in_index1 + 0;
      _cache[out_index + 2] = in_index0 + 1;
      _cache[out_index + 3] = in_index1 + 1;
      _cache[out_index + 4] = in_index0 + 2;
      _cache[out_index + 5] = in_index1 + 2;
      _cache[out_index + 6] = in_index0 + 3;
      _cache[out_index + 7] = in_index1 + 3;
    }

    // Patches soa tracks, reuse previous track. This prevents from filling
    // animation with useless keyframes.
    for (int i = num_animated_tracks; i < num_animated_soa_tracks * 4; ++i) {
      const int out_index = i * 2;
      _cache[out_index + 0] = _cache[out_index - 2];
      _cache[out_index + 1] = _cache[out_index - 1];
    }

    cursor = _keys.begin + num_animated_tracks * 2;  // New cursor position.

    // All entries are outdated. It cares to only flag valid soa entries as
    // this is the exit condition of other algorithms.
    const int num_outdated_flags = (num_animated_soa_tracks + 7) / 8;
    for (int i = 0; i < num_outdated_flags - 1; ++i) {
      _outdated[i] = 0xff;
    }
    _outdated[num_outdated_flags - 1] =
        0xff >> (num_outdated_flags * 8 - num_animated_soa_tracks);
  } else {
    // TODO
    // assert(cursor >= _keys.begin + num_tracks * 2 && cursor <= _keys.end);
  }

  // Search for the keys that matches _ratio.
  // Iterates while the cache is not updated with left and right keys required
  // for interpolation at time ratio _ratio, for all tracks. Thanks to the
  // keyframe sorting, the loop can end as soon as it finds a key greater that
  // _ratio. It will mean that all the keys lower than _ratio have been
  // processed, meaning all cache entries are up to date.
  while (cursor < _keys.end &&
         _keys[_cache[cursor->track * 2 + 1]].ratio <= _ratio) {
    // Flag this soa entry as outdated.
    _outdated[cursor->track / 32] |= (1 << ((cursor->track & 0x1f) / 4));
    // Updates cache.
    const int base = cursor->track * 2;
    _cache[base] = _cache[base + 1];
    _cache[base + 1] = static_cast<int>(cursor - _keys.begin);
    // Process next key.
    ++cursor;
  }
  assert(cursor <= _keys.end);

  // Updates cursor output.
  *_cursor = static_cast<int>(cursor - _keys.begin);
}

template <typename _Key, typename _InterpKey, typename _Decompress>
void UpdateCacheKeyframes(int _num_soa_tracks, ozz::Range<const _Key> _keys,
                          const int* _interp, uint8_t* _outdated,
                          _InterpKey* _interp_keys,
                          const _Decompress& _decompress) {
  const int num_outdated_flags = (_num_soa_tracks + 7) / 8;
  for (int j = 0; j < num_outdated_flags; ++j) {
    uint8_t outdated = _outdated[j];
    _outdated[j] = 0;  // Reset outdated entries as all will be processed.
    for (int i = j * 8; outdated; ++i, outdated >>= 1) {
      if (!(outdated & 1)) {
        continue;
      }
      const int base = i * 4 * 2;  // * soa size * 2 keys

      // Decompress left side keyframes and store them in soa structures.
      const _Key& k00 = _keys.begin[_interp[base + 0]];
      const _Key& k10 = _keys.begin[_interp[base + 2]];
      const _Key& k20 = _keys.begin[_interp[base + 4]];
      const _Key& k30 = _keys.begin[_interp[base + 6]];
      _interp_keys[i].ratio[0] =
          math::simd_float4::Load(k00.ratio, k10.ratio, k20.ratio, k30.ratio);
      _decompress(k00, k10, k20, k30, &_interp_keys[i].value[0]);

      // Decompress right side keyframes and store them in soa structures.
      const _Key& k01 = _keys.begin[_interp[base + 1]];
      const _Key& k11 = _keys.begin[_interp[base + 3]];
      const _Key& k21 = _keys.begin[_interp[base + 5]];
      const _Key& k31 = _keys.begin[_interp[base + 7]];
      _interp_keys[i].ratio[1] =
          math::simd_float4::Load(k01.ratio, k11.ratio, k21.ratio, k31.ratio);
      _decompress(k01, k11, k21, k31, &_interp_keys[i].value[1]);
    }
  }
}

inline void DecompressFloat3(const Float3Key& _k0, const Float3Key& _k1,
                             const Float3Key& _k2, const Float3Key& _k3,
                             math::SoaFloat3* _float3) {
  _float3->x = math::HalfToFloat(math::simd_int4::Load(
      _k0.value[0], _k1.value[0], _k2.value[0], _k3.value[0]));
  _float3->y = math::HalfToFloat(math::simd_int4::Load(
      _k0.value[1], _k1.value[1], _k2.value[1], _k3.value[1]));
  _float3->z = math::HalfToFloat(math::simd_int4::Load(
      _k0.value[2], _k1.value[2], _k2.value[2], _k3.value[2]));
}

// Defines a mapping table that defines components assignation in the output
// quaternion.
constexpr int kCpntMapping[4][4] = {
    {0, 0, 1, 2}, {0, 0, 1, 2}, {0, 1, 0, 2}, {0, 1, 2, 0}};

inline void DecompressQuaternion(const QuaternionKey& _k0,
                                 const QuaternionKey& _k1,
                                 const QuaternionKey& _k2,
                                 const QuaternionKey& _k3,
                                 math::SoaQuaternion* _quaternion) {
  // Selects proper mapping for each key.
  const int* m0 = kCpntMapping[_k0.largest];
  const int* m1 = kCpntMapping[_k1.largest];
  const int* m2 = kCpntMapping[_k2.largest];
  const int* m3 = kCpntMapping[_k3.largest];

  // Prepares an array of input values, according to the mapping required to
  // restore quaternion largest component.
  alignas(16) int cmp_keys[4][4] = {
      {_k0.value[m0[0]], _k1.value[m1[0]], _k2.value[m2[0]], _k3.value[m3[0]]},
      {_k0.value[m0[1]], _k1.value[m1[1]], _k2.value[m2[1]], _k3.value[m3[1]]},
      {_k0.value[m0[2]], _k1.value[m1[2]], _k2.value[m2[2]], _k3.value[m3[2]]},
      {_k0.value[m0[3]], _k1.value[m1[3]], _k2.value[m2[3]], _k3.value[m3[3]]},
  };

  // Resets largest component to 0. Overwritting here avoids 16 branchings
  // above.
  cmp_keys[_k0.largest][0] = 0;
  cmp_keys[_k1.largest][1] = 0;
  cmp_keys[_k2.largest][2] = 0;
  cmp_keys[_k3.largest][3] = 0;

  // Rebuilds quaternion from quantized values.
  const math::SimdFloat4 kInt2Float =
      math::simd_float4::Load1(1.f / (32767.f * math::kSqrt2));
  math::SimdFloat4 cpnt[4] = {
      kInt2Float *
          math::simd_float4::FromInt(math::simd_int4::LoadPtr(cmp_keys[0])),
      kInt2Float *
          math::simd_float4::FromInt(math::simd_int4::LoadPtr(cmp_keys[1])),
      kInt2Float *
          math::simd_float4::FromInt(math::simd_int4::LoadPtr(cmp_keys[2])),
      kInt2Float *
          math::simd_float4::FromInt(math::simd_int4::LoadPtr(cmp_keys[3])),
  };

  // Get back length of 4th component. Favors performance over accuracy by using
  // x * RSqrtEst(x) instead of Sqrt(x).
  // ww0 cannot be 0 because we 're recomputing the largest component.
  const math::SimdFloat4 dot = cpnt[0] * cpnt[0] + cpnt[1] * cpnt[1] +
                               cpnt[2] * cpnt[2] + cpnt[3] * cpnt[3];
  const math::SimdFloat4 ww0 = math::Max(math::simd_float4::Load1(1e-16f),
                                         math::simd_float4::one() - dot);
  const math::SimdFloat4 w0 = ww0 * math::RSqrtEst(ww0);
  // Re-applies 4th component' s sign.
  const math::SimdInt4 sign = math::ShiftL(
      math::simd_int4::Load(_k0.sign, _k1.sign, _k2.sign, _k3.sign), 31);
  const math::SimdFloat4 restored = math::Or(w0, sign);

  // Re-injects the largest component inside the SoA structure.
  cpnt[_k0.largest] = math::Or(
      cpnt[_k0.largest], math::And(restored, math::simd_int4::mask_f000()));
  cpnt[_k1.largest] = math::Or(
      cpnt[_k1.largest], math::And(restored, math::simd_int4::mask_0f00()));
  cpnt[_k2.largest] = math::Or(
      cpnt[_k2.largest], math::And(restored, math::simd_int4::mask_00f0()));
  cpnt[_k3.largest] = math::Or(
      cpnt[_k3.largest], math::And(restored, math::simd_int4::mask_000f()));

  // Stores result.
  _quaternion->x = cpnt[0];
  _quaternion->y = cpnt[1];
  _quaternion->z = cpnt[2];
  _quaternion->w = cpnt[3];
}

void Interpolate(float _anim_ratio, size_t _num_soa_tracks,
                 const internal::InterpSoaFloat3* _interp_soa,
                 const Range<uint8_t>& _types, math::SoaFloat3* _output,
                 size_t _stride) {
  /* TODO
   assert(_translations.count() == _translations_map.count() &&
   _rotations.count() == _rotations_map.count() &&
   _scales.count() == _scales_map.count());
   */
  const math::SimdFloat4 anim_ratio = math::simd_float4::Load1(_anim_ratio);

  Typer typer(_types, TrackType::kAnimated);
  for (size_t i = 0; i < _num_soa_tracks; ++i) {
    const math::SimdFloat4 ratio =
        (anim_ratio - _interp_soa[i].ratio[0]) *
        math::RcpEst(_interp_soa[i].ratio[1] - _interp_soa[i].ratio[0]);
    *PointerStride(_output, (++typer) * _stride) =
        Lerp(_interp_soa[i].value[0], _interp_soa[i].value[1], ratio);
  }
}

void Interpolate(float _anim_ratio, size_t _num_soa_tracks,
                 const internal::InterpSoaQuaternion* _interp_soa,
                 const Range<uint8_t>& _types, math::SoaQuaternion* _output,
                 size_t _stride) {
  const math::SimdFloat4 anim_ratio = math::simd_float4::Load1(_anim_ratio);

  Typer typer(_types, TrackType::kAnimated);
  for (size_t i = 0; i < _num_soa_tracks; ++i) {
    const math::SimdFloat4 ratio =
        (anim_ratio - _interp_soa[i].ratio[0]) *
        math::RcpEst(_interp_soa[i].ratio[1] - _interp_soa[i].ratio[0]);
    *PointerStride(_output, (++typer) * _stride) =
        NLerpEst(_interp_soa[i].value[0], _interp_soa[i].value[1], ratio);
  }
}

void CopyConst(const Range<const Float3ConstKey>& _constkeys,
               const Range<uint8_t>& _types, math::SoaFloat3* _output,
               size_t _stride) {
  size_t s = _constkeys.count();
  if (s == 0) {
    return;
  }
  math::SimdFloat4 aos[4];
  Typer typer(_types, TrackType::kConstant);
  for (size_t i = 0; i < _constkeys.count(); i += 4) {
    aos[0] = math::simd_float4::Load3PtrU(_constkeys[i + 0].values);
    aos[1] = math::simd_float4::Load3PtrU(_constkeys[i + 1].values);
    aos[2] = math::simd_float4::Load3PtrU(_constkeys[i + 2].values);
    aos[3] = math::simd_float4::Load3PtrU(_constkeys[i + 3].values);

    math::SoaFloat3& output = *PointerStride(_output, (++typer) * _stride);
    math::Transpose4x3(aos, &output.x);
  }
}

void CopyConst(const Range<const QuaternionConstKey>& _constkeys,
               const Range<uint8_t>& _types, math::SoaQuaternion* _output,
               size_t _stride) {
  if (_constkeys.count() == 0) {
    return;
  }
  math::SimdFloat4 aos[4];
  Typer typer(_types, TrackType::kConstant);
  for (size_t i = 0; i < _constkeys.count(); i += 4) {
    aos[0] = math::simd_float4::LoadPtrU(_constkeys[i + 0].values);
    aos[1] = math::simd_float4::LoadPtrU(_constkeys[i + 1].values);
    aos[2] = math::simd_float4::LoadPtrU(_constkeys[i + 2].values);
    aos[3] = math::simd_float4::LoadPtrU(_constkeys[i + 3].values);

    math::SoaQuaternion& output = *PointerStride(_output, (++typer) * _stride);
    math::Transpose4x4(aos, &output.x);
  }
}

void CopyIdentity(int _soa_translastion,
                  const Range<uint8_t>& _translation_types, int _soa_rotation,
                  const Range<uint8_t>& _rotation_types, int _soa_scale,
                  const Range<uint8_t>& _scale_types,
                  const Range<math::SoaTransform>& _output) {
  const math::SimdFloat4 zero = math::simd_float4::zero();
  const math::SimdFloat4 one = math::simd_float4::one();

  Typer ttyper(_translation_types, TrackType::kIdentity);
  for (int i = 0; i < _soa_translastion; ++i) {
    math::SoaFloat3& translation = _output[++ttyper].translation;
    translation.x = translation.y = translation.z = zero;
  }
  Typer rtyper(_rotation_types, TrackType::kIdentity);
  for (int i = 0; i < _soa_rotation; ++i) {
    math::SoaQuaternion& rotation = _output[++rtyper].rotation;
    rotation.x = rotation.y = rotation.z = zero;
    rotation.w = one;
  }
  Typer styper(_scale_types, TrackType::kIdentity);
  for (int i = 0; i < _soa_scale; ++i) {
    math::SoaFloat3& scale = _output[++styper].scale;
    scale.x = scale.y = scale.z = one;
  }
}
}  // namespace

SamplingJob::SamplingJob() : ratio(0.f), animation(nullptr), cache(nullptr) {}

bool SamplingJob::Run() const {
  if (!Validate()) {
    return false;
  }

  const int num_tracks = animation->num_tracks();
  if (num_tracks == 0) {  // Early out if animation contains no joint.
    return true;
  }

  // Clamps ratio in range [0,duration].
  const float anim_ratio = math::Clamp(0.f, ratio, 1.f);

  // Step the cache to this potentially new animation and ratio.
  cache->Step(*animation, anim_ratio);

  // Fetch key frames from the animation to the cache at r = anim_ratio.
  // Then updates outdated soa hot values.
  UpdateCacheCursor(anim_ratio, animation->translations(),
                    &cache->translation_cursor_, cache->translation_keys_,
                    cache->outdated_translations_,
                    &cache->animated_translations_soa_count_);
  UpdateCacheKeyframes(cache->animated_translations_soa_count_,
                       animation->translations(), cache->translation_keys_,
                       cache->outdated_translations_, cache->soa_translations_,
                       &DecompressFloat3);

  UpdateCacheCursor(anim_ratio, animation->rotations(),
                    &cache->rotation_cursor_, cache->rotation_keys_,
                    cache->outdated_rotations_,
                    &cache->animated_rotations_soa_count_);
  UpdateCacheKeyframes(cache->animated_rotations_soa_count_,
                       animation->rotations(), cache->rotation_keys_,
                       cache->outdated_rotations_, cache->soa_rotations_,
                       &DecompressQuaternion);

  UpdateCacheCursor(anim_ratio, animation->scales(), &cache->scale_cursor_,
                    cache->scale_keys_, cache->outdated_scales_,
                    &cache->animated_scales_soa_count_);
  UpdateCacheKeyframes(cache->animated_scales_soa_count_, animation->scales(),
                       cache->scale_keys_, cache->outdated_scales_,
                       cache->soa_scales_, &DecompressFloat3);

  // Interpolates soa hot data.
  Interpolate(anim_ratio, cache->animated_translations_soa_count_,
              cache->soa_translations_, animation->translation_types,
              &output.begin->translation, sizeof(math::SoaTransform));
  Interpolate(anim_ratio, cache->animated_rotations_soa_count_,
              cache->soa_rotations_, animation->rotation_types,
              &output.begin->rotation, sizeof(math::SoaTransform));
  Interpolate(anim_ratio, cache->animated_scales_soa_count_, cache->soa_scales_,
              animation->scale_types, &output.begin->scale,
              sizeof(math::SoaTransform));

  CopyConst(animation->const_translations_, animation->translation_types,
            &output.begin->translation, sizeof(math::SoaTransform));
  CopyConst(animation->const_rotations_, animation->rotation_types,
            &output.begin->rotation, sizeof(math::SoaTransform));
  CopyConst(animation->const_scales_, animation->scale_types,
            &output.begin->scale, sizeof(math::SoaTransform));

  const int num_soa_tracks = animation->num_soa_tracks();
  const int num_identity_soa_translation =
      num_soa_tracks - (animation->const_translations_.count() + 3) / 4 -
      cache->animated_translations_soa_count_;
  const int num_identity_soa_rotation =
      num_soa_tracks - (animation->const_rotations_.count() + 3) / 4 -
      cache->animated_rotations_soa_count_;
  const int num_identity_soa_scale =
      num_soa_tracks - (animation->const_scales_.count() + 3) / 4 -
      cache->animated_scales_soa_count_;
  CopyIdentity(num_identity_soa_translation, animation->translation_types,
               num_identity_soa_rotation, animation->rotation_types,
               num_identity_soa_scale, animation->scale_types, output);

  return true;
}

SamplingCache::SamplingCache()
    : max_soa_tracks_(0),
      soa_translations_(
          nullptr) {  // soa_translations_ is the allocation pointer.
  Invalidate();
}

SamplingCache::SamplingCache(int _max_tracks)
    : max_soa_tracks_(0),
      soa_translations_(
          nullptr) {  // soa_translations_ is the allocation pointer.
  Resize(_max_tracks);
}

SamplingCache::~SamplingCache() {
  // Deallocates everything at once.
  memory::default_allocator()->Deallocate(soa_translations_);
}

void SamplingCache::Resize(int _max_tracks) {
  using internal::InterpSoaFloat3;
  using internal::InterpSoaQuaternion;

  // Reset existing data.
  Invalidate();

  // Updates maximum supported soa tracks.
  max_soa_tracks_ = (_max_tracks + 3) / 4;

  // Allocate all cache data at once in a single allocation.
  // Alignment is guaranteed because memory is dispatch from the highest
  // alignment requirement (Soa data: SimdFloat4) to the lowest (outdated
  // flag: unsigned char).

  // Computes allocation size.
  const size_t max_tracks = max_soa_tracks_ * 4;
  const size_t num_outdated = (max_soa_tracks_ + 7) / 8;
  const size_t size =
      sizeof(InterpSoaFloat3) * max_soa_tracks_ +
      sizeof(InterpSoaQuaternion) * max_soa_tracks_ +
      sizeof(InterpSoaFloat3) * max_soa_tracks_ +
      sizeof(int) * max_tracks * 2 * 3 +  // 2 keys * (trans + rot + scale).
      sizeof(uint8_t) * 3 * num_outdated;

  // Allocates all at once.
  memory::Allocator* allocator = memory::default_allocator();
  char* alloc_begin = reinterpret_cast<char*>(
      allocator->Reallocate(soa_translations_, size, alignof(InterpSoaFloat3)));
  char* alloc_cursor = alloc_begin;

  // Distributes buffer memory while ensuring proper alignment (serves larger
  // alignment values first).
  static_assert(alignof(InterpSoaFloat3) >= alignof(InterpSoaQuaternion) &&
                    alignof(InterpSoaQuaternion) >= alignof(InterpSoaFloat3) &&
                    alignof(InterpSoaFloat3) >= alignof(int) &&
                    alignof(int) >= alignof(uint8_t),
                "Must serve larger alignment values first)");

  soa_translations_ = reinterpret_cast<InterpSoaFloat3*>(alloc_cursor);
  assert(math::IsAligned(soa_translations_, alignof(InterpSoaFloat3)));
  alloc_cursor += sizeof(InterpSoaFloat3) * max_soa_tracks_;
  soa_rotations_ = reinterpret_cast<InterpSoaQuaternion*>(alloc_cursor);
  assert(math::IsAligned(soa_rotations_, alignof(InterpSoaQuaternion)));
  alloc_cursor += sizeof(InterpSoaQuaternion) * max_soa_tracks_;
  soa_scales_ = reinterpret_cast<InterpSoaFloat3*>(alloc_cursor);
  assert(math::IsAligned(soa_scales_, alignof(InterpSoaFloat3)));
  alloc_cursor += sizeof(InterpSoaFloat3) * max_soa_tracks_;

  translation_keys_ = reinterpret_cast<int*>(alloc_cursor);
  assert(math::IsAligned(translation_keys_, alignof(int)));
  alloc_cursor += sizeof(int) * max_tracks * 2;
  rotation_keys_ = reinterpret_cast<int*>(alloc_cursor);
  alloc_cursor += sizeof(int) * max_tracks * 2;
  scale_keys_ = reinterpret_cast<int*>(alloc_cursor);
  alloc_cursor += sizeof(int) * max_tracks * 2;

  outdated_translations_ = reinterpret_cast<uint8_t*>(alloc_cursor);
  assert(math::IsAligned(outdated_translations_, alignof(uint8_t)));
  alloc_cursor += sizeof(uint8_t) * num_outdated;
  outdated_rotations_ = reinterpret_cast<uint8_t*>(alloc_cursor);
  alloc_cursor += sizeof(uint8_t) * num_outdated;
  outdated_scales_ = reinterpret_cast<uint8_t*>(alloc_cursor);
  alloc_cursor += sizeof(uint8_t) * num_outdated;

  assert(alloc_cursor == alloc_begin + size);
}

void SamplingCache::Step(const Animation& _animation, float _ratio) {
  // The cache is invalidated if animation has changed or if it is being rewind.
  if (animation_ != &_animation || _ratio < ratio_) {
    animation_ = &_animation;
    translation_cursor_ = 0;
    rotation_cursor_ = 0;
    scale_cursor_ = 0;
  }
  ratio_ = _ratio;
}

void SamplingCache::Invalidate() {
  animation_ = nullptr;
  ratio_ = 0.f;
  translation_cursor_ = 0;
  rotation_cursor_ = 0;
  scale_cursor_ = 0;
}
}  // namespace animation
}  // namespace ozz
