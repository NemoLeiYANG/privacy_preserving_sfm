// Copyright (c) 2020, ETH Zurich and UNC Chapel Hill.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//
//     * Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//
//     * Neither the name of ETH Zurich and UNC Chapel Hill nor the names of
//       its contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Authors: Johannes L. Schoenberger (jsch-at-demuc-dot-de)
//          Viktor Larsson (viktor.larsson@inf.ethz.ch)
//          Marcel Geppert (marcel.geppert@inf.ethz.ch)

#include "feature/matching.h"


#include <fstream>
#include <numeric>
#include <unordered_set>

#include "SiftGPU/SiftGPU.h"
#include <FLANN/algorithms/dist.h>
#include <FLANN/algorithms/linear_index.h>
#include <FLANN/util/matrix.h>
#include "base/gps.h"
#include "feature/utils.h"
#include "util/cuda.h"
#include "util/misc.h"

namespace colmap {
namespace {

void PrintElapsedTime(const Timer& timer) {
  std::cout << StringPrintf(" in %.3fs", timer.ElapsedSeconds()) << std::endl;
}
}  // namespace

bool ExhaustiveMatchingOptions::Check() const {
  CHECK_OPTION_GT(block_size, 1);
  return true;
}

bool SequentialMatchingOptions::Check() const {
  CHECK_OPTION_GT(overlap, 0);
  return true;
}

bool SpatialMatchingOptions::Check() const {
  CHECK_OPTION_GT(max_num_neighbors, 0);
  CHECK_OPTION_GT(max_distance, 0.0);
  return true;
}

bool TransitiveMatchingOptions::Check() const {
  CHECK_OPTION_GT(batch_size, 0);
  CHECK_OPTION_GT(num_iterations, 0);
  return true;
}

bool ImagePairsMatchingOptions::Check() const {
  CHECK_OPTION_GT(block_size, 0);
  return true;
}

bool FeaturePairsMatchingOptions::Check() const { return true; }

FeatureMatcherCache::FeatureMatcherCache(const size_t cache_size,
                                         const Database* database)
    : cache_size_(cache_size), database_(database) {
  CHECK_NOTNULL(database_);
}

void FeatureMatcherCache::Setup() {
  const std::vector<Camera> cameras = database_->ReadAllCameras();
  cameras_cache_.reserve(cameras.size());
  for (const auto& camera : cameras) {
    cameras_cache_.emplace(camera.CameraId(), camera);
  }

  const std::vector<Image> images = database_->ReadAllImages();
  images_cache_.reserve(images.size());
  for (const auto& image : images) {
    images_cache_.emplace(image.ImageId(), image);
  }

  descriptors_cache_.reset(new LRUCache<image_t, FeatureDescriptors>(
      cache_size_, [this](const image_t image_id) {
        return database_->ReadDescriptors(image_id);
      }));
}

const Camera& FeatureMatcherCache::GetCamera(const camera_t camera_id) const {
  return cameras_cache_.at(camera_id);
}

const Image& FeatureMatcherCache::GetImage(const image_t image_id) const {
  return images_cache_.at(image_id);
}

const FeatureDescriptors& FeatureMatcherCache::GetDescriptors(
    const image_t image_id) {
  std::unique_lock<std::mutex> lock(database_mutex_);
  return descriptors_cache_->Get(image_id);
}

FeatureMatches FeatureMatcherCache::GetMatches(const image_t image_id1,
                                               const image_t image_id2) {
  std::unique_lock<std::mutex> lock(database_mutex_);
  return database_->ReadMatches(image_id1, image_id2);
}

std::vector<image_t> FeatureMatcherCache::GetImageIds() const {
  std::vector<image_t> image_ids;
  image_ids.reserve(images_cache_.size());
  for (const auto& image : images_cache_) {
    image_ids.push_back(image.first);
  }
  return image_ids;
}

bool FeatureMatcherCache::ExistsMatches(const image_t image_id1,
                                        const image_t image_id2) {
  std::unique_lock<std::mutex> lock(database_mutex_);
  return database_->ExistsMatches(image_id1, image_id2);
}

void FeatureMatcherCache::WriteMatches(const image_t image_id1,
                                       const image_t image_id2,
                                       const FeatureMatches& matches) {
  std::unique_lock<std::mutex> lock(database_mutex_);
  database_->WriteMatches(image_id1, image_id2, matches);
}

void FeatureMatcherCache::DeleteMatches(const image_t image_id1,
                                        const image_t image_id2) {
  std::unique_lock<std::mutex> lock(database_mutex_);
  database_->DeleteMatches(image_id1, image_id2);
}

FeatureMatcherThread::FeatureMatcherThread(const SiftMatchingOptions& options,
                                           FeatureMatcherCache* cache)
    : options_(options), cache_(cache) {}

void FeatureMatcherThread::SetMaxNumMatches(const int max_num_matches) {
  options_.max_num_matches = max_num_matches;
}

SiftCPUFeatureMatcher::SiftCPUFeatureMatcher(const SiftMatchingOptions& options,
                                             FeatureMatcherCache* cache,
                                             JobQueue<Input>* input_queue,
                                             JobQueue<Output>* output_queue)
    : FeatureMatcherThread(options, cache),
      input_queue_(input_queue),
      output_queue_(output_queue) {
  CHECK(options_.Check());
}

void SiftCPUFeatureMatcher::Run() {
  SignalValidSetup();

  while (true) {
    if (IsStopped()) {
      break;
    }

    const auto input_job = input_queue_->Pop();
    if (input_job.IsValid()) {
      auto data = input_job.Data();

      const FeatureDescriptors descriptors1 =
          cache_->GetDescriptors(data.image_id1);
      const FeatureDescriptors descriptors2 =
          cache_->GetDescriptors(data.image_id2);
      MatchSiftFeaturesCPU(options_, descriptors1, descriptors2, &data.matches);

      if (data.matches.size() < options_.min_num_matches) {
        data.matches = {};
      }

      CHECK(output_queue_->Push(data));
    }
  }
}

SiftGPUFeatureMatcher::SiftGPUFeatureMatcher(const SiftMatchingOptions& options,
                                             FeatureMatcherCache* cache,
                                             JobQueue<Input>* input_queue,
                                             JobQueue<Output>* output_queue)
    : FeatureMatcherThread(options, cache),
      input_queue_(input_queue),
      output_queue_(output_queue) {
  CHECK(options_.Check());

  prev_uploaded_image_ids_[0] = kInvalidImageId;
  prev_uploaded_image_ids_[1] = kInvalidImageId;

#ifndef CUDA_ENABLED
  opengl_context_.reset(new OpenGLContextManager());
#endif
}

void SiftGPUFeatureMatcher::Run() {
#ifndef CUDA_ENABLED
  CHECK(opengl_context_);
  opengl_context_->MakeCurrent();
#endif

  SiftMatchGPU sift_match_gpu;
  if (!CreateSiftGPUMatcher(options_, &sift_match_gpu)) {
    std::cout << "ERROR: SiftGPU not fully supported" << std::endl;
    SignalInvalidSetup();
    return;
  }

  SignalValidSetup();

  while (true) {
    if (IsStopped()) {
      break;
    }

    const auto input_job = input_queue_->Pop();
    if (input_job.IsValid()) {
      auto data = input_job.Data();

      const FeatureDescriptors* descriptors1_ptr;
      GetDescriptorData(0, data.image_id1, &descriptors1_ptr);
      const FeatureDescriptors* descriptors2_ptr;
      GetDescriptorData(1, data.image_id2, &descriptors2_ptr);
      MatchSiftFeaturesGPU(options_, descriptors1_ptr, descriptors2_ptr,
                           &sift_match_gpu, &data.matches);

      if (data.matches.size() < options_.min_num_matches) {
        data.matches = {};
      }

      CHECK(output_queue_->Push(data));
    }
  }
}

void SiftGPUFeatureMatcher::GetDescriptorData(
    const int index, const image_t image_id,
    const FeatureDescriptors** descriptors_ptr) {
  CHECK_GE(index, 0);
  CHECK_LE(index, 1);
  if (prev_uploaded_image_ids_[index] == image_id) {
    *descriptors_ptr = nullptr;
  } else {
    prev_uploaded_descriptors_[index] = cache_->GetDescriptors(image_id);
    *descriptors_ptr = &prev_uploaded_descriptors_[index];
    prev_uploaded_image_ids_[index] = image_id;
  }
}

SiftFeatureMatcher::SiftFeatureMatcher(const SiftMatchingOptions& options,
                                       Database* database,
                                       FeatureMatcherCache* cache)
    : options_(options), database_(database), cache_(cache), is_setup_(false) {
  CHECK(options_.Check());

  const int num_threads = GetEffectiveNumThreads(options_.num_threads);
  CHECK_GT(num_threads, 0);

  std::vector<int> gpu_indices = CSVToVector<int>(options_.gpu_index);
  CHECK_GT(gpu_indices.size(), 0);

#ifdef CUDA_ENABLED
  if (options_.use_gpu && gpu_indices.size() == 1 && gpu_indices[0] == -1) {
    const int num_cuda_devices = GetNumCudaDevices();
    CHECK_GT(num_cuda_devices, 0);
    gpu_indices.resize(num_cuda_devices);
    std::iota(gpu_indices.begin(), gpu_indices.end(), 0);
  }
#endif  // CUDA_ENABLED

  if (options_.use_gpu) {
    auto gpu_options = options_;
    matchers_.reserve(gpu_indices.size());
    for (const auto& gpu_index : gpu_indices) {
      gpu_options.gpu_index = std::to_string(gpu_index);
      matchers_.emplace_back(new SiftGPUFeatureMatcher(
          gpu_options, cache, &matcher_queue_, &output_queue_));
    }
  } else {
    matchers_.reserve(num_threads);
    for (int i = 0; i < num_threads; ++i) {
      matchers_.emplace_back(new SiftCPUFeatureMatcher(
          options_, cache, &matcher_queue_, &output_queue_));
    }
  }
}

SiftFeatureMatcher::~SiftFeatureMatcher() {
  matcher_queue_.Wait();
  output_queue_.Wait();

  for (auto& matcher : matchers_) {
    matcher->Stop();
  }

  matcher_queue_.Stop();
  output_queue_.Stop();

  for (auto& matcher : matchers_) {
    matcher->Wait();
  }
}

bool SiftFeatureMatcher::Setup() {
  const int max_num_features = CHECK_NOTNULL(database_)->MaxNumDescriptors();
  options_.max_num_matches =
      std::min(options_.max_num_matches, max_num_features);

  for (auto& matcher : matchers_) {
    matcher->SetMaxNumMatches(options_.max_num_matches);
    matcher->Start();
  }

  for (auto& matcher : matchers_) {
    if (!matcher->CheckValidSetup()) {
      return false;
    }
  }

  is_setup_ = true;

  return true;
}

void SiftFeatureMatcher::Match(
    const std::vector<std::pair<image_t, image_t>>& image_pairs) {
  CHECK_NOTNULL(database_);
  CHECK_NOTNULL(cache_);
  CHECK(is_setup_);

  if (image_pairs.empty()) {
    return;
  }

  //////////////////////////////////////////////////////////////////////////////
  // Match the image pairs
  //////////////////////////////////////////////////////////////////////////////

  std::unordered_set<image_pair_t> image_pair_ids;
  image_pair_ids.reserve(image_pairs.size());

  size_t num_outputs = 0;
  for (const auto image_pair : image_pairs) {
    // Avoid self-matches.
    if (image_pair.first == image_pair.second) {
      continue;
    }

    // Avoid duplicate image pairs.
    const image_pair_t pair_id =
        Database::ImagePairToPairId(image_pair.first, image_pair.second);
    if (image_pair_ids.count(pair_id) > 0) {
      continue;
    }

    image_pair_ids.insert(pair_id);

    const bool exists_matches =
        cache_->ExistsMatches(image_pair.first, image_pair.second);

    if (exists_matches) {
      continue;
    }

    num_outputs += 1;

    // If only one of the matches or inlier matches exist, we recompute them
    // from scratch and delete the existing results. This must be done before
    // pushing the jobs to the queue, otherwise database constraints might fail
    // when writing an existing result into the database.

    internal::FeatureMatcherData data;
    data.image_id1 = image_pair.first;
    data.image_id2 = image_pair.second;

    CHECK(matcher_queue_.Push(data));
  }

  //////////////////////////////////////////////////////////////////////////////
  // Write results to database
  //////////////////////////////////////////////////////////////////////////////

  for (size_t i = 0; i < num_outputs; ++i) {
    const auto output_job = output_queue_.Pop();
    CHECK(output_job.IsValid());
    auto output = output_job.Data();

    if (output.matches.size() < static_cast<size_t>(options_.min_num_matches)) {
      output.matches = {};
    }

    cache_->WriteMatches(output.image_id1, output.image_id2, output.matches);
  }

  CHECK_EQ(output_queue_.Size(), 0);
}

ExhaustiveFeatureMatcher::ExhaustiveFeatureMatcher(
    const ExhaustiveMatchingOptions& options,
    const SiftMatchingOptions& match_options, const std::string& database_path)
    : options_(options),
      match_options_(match_options),
      database_(database_path),
      cache_(5 * options_.block_size, &database_),
      matcher_(match_options, &database_, &cache_) {
  CHECK(options_.Check());
  CHECK(match_options_.Check());
}

void ExhaustiveFeatureMatcher::Run() {
  PrintHeading1("Exhaustive feature matching");

  if (!matcher_.Setup()) {
    return;
  }

  cache_.Setup();

  const std::vector<image_t> image_ids = cache_.GetImageIds();

  const size_t block_size = static_cast<size_t>(options_.block_size);
  const size_t num_blocks = static_cast<size_t>(
      std::ceil(static_cast<double>(image_ids.size()) / block_size));
  const size_t num_pairs_per_block = block_size * (block_size - 1) / 2;

  std::vector<std::pair<image_t, image_t>> image_pairs;
  image_pairs.reserve(num_pairs_per_block);

  for (size_t start_idx1 = 0; start_idx1 < image_ids.size();
       start_idx1 += block_size) {
    const size_t end_idx1 =
        std::min(image_ids.size(), start_idx1 + block_size) - 1;
    for (size_t start_idx2 = 0; start_idx2 < image_ids.size();
         start_idx2 += block_size) {
      const size_t end_idx2 =
          std::min(image_ids.size(), start_idx2 + block_size) - 1;

      if (IsStopped()) {
        GetTimer().PrintMinutes();
        return;
      }

      Timer timer;
      timer.Start();

      std::cout << StringPrintf("Matching block [%d/%d, %d/%d]",
                                start_idx1 / block_size + 1, num_blocks,
                                start_idx2 / block_size + 1, num_blocks)
                << std::flush;

      image_pairs.clear();
      for (size_t idx1 = start_idx1; idx1 <= end_idx1; ++idx1) {
        for (size_t idx2 = start_idx2; idx2 <= end_idx2; ++idx2) {
          const size_t block_id1 = idx1 % block_size;
          const size_t block_id2 = idx2 % block_size;
          if ((idx1 > idx2 && block_id1 <= block_id2) ||
              (idx1 < idx2 &&
               block_id1 < block_id2)) {  // Avoid duplicate pairs
            image_pairs.emplace_back(image_ids[idx1], image_ids[idx2]);
          }
        }
      }

      DatabaseTransaction database_transaction(&database_);
      matcher_.Match(image_pairs);

      PrintElapsedTime(timer);
    }
  }

  GetTimer().PrintMinutes();
}

SequentialFeatureMatcher::SequentialFeatureMatcher(
    const SequentialMatchingOptions& options,
    const SiftMatchingOptions& match_options, const std::string& database_path)
    : options_(options),
      match_options_(match_options),
      database_(database_path),
      cache_(5 * options_.overlap, &database_),
      matcher_(match_options, &database_, &cache_) {
  CHECK(options_.Check());
  CHECK(match_options_.Check());
}

void SequentialFeatureMatcher::Run() {
  PrintHeading1("Sequential feature matching");

  if (!matcher_.Setup()) {
    return;
  }

  cache_.Setup();

  const std::vector<image_t> ordered_image_ids = GetOrderedImageIds();

  RunSequentialMatching(ordered_image_ids);

  GetTimer().PrintMinutes();
}

std::vector<image_t> SequentialFeatureMatcher::GetOrderedImageIds() const {
  const std::vector<image_t> image_ids = cache_.GetImageIds();

  std::vector<Image> ordered_images;
  ordered_images.reserve(image_ids.size());
  for (const auto image_id : image_ids) {
    ordered_images.push_back(cache_.GetImage(image_id));
  }

  std::sort(ordered_images.begin(), ordered_images.end(),
            [](const Image& image1, const Image& image2) {
              return image1.Name() < image2.Name();
            });

  std::vector<image_t> ordered_image_ids;
  ordered_image_ids.reserve(image_ids.size());
  for (const auto& image : ordered_images) {
    ordered_image_ids.push_back(image.ImageId());
  }

  return ordered_image_ids;
}

void SequentialFeatureMatcher::RunSequentialMatching(
    const std::vector<image_t>& image_ids) {
  std::vector<std::pair<image_t, image_t>> image_pairs;
  image_pairs.reserve(options_.overlap);

  for (size_t image_idx1 = 0; image_idx1 < image_ids.size(); ++image_idx1) {
    if (IsStopped()) {
      return;
    }

    const auto image_id1 = image_ids.at(image_idx1);

    Timer timer;
    timer.Start();

    std::cout << StringPrintf("Matching image [%d/%d]", image_idx1 + 1,
                              image_ids.size())
              << std::flush;

    image_pairs.clear();
    for (int i = 0; i < options_.overlap; ++i) {
      const size_t image_idx2 = image_idx1 + i;
      if (image_idx2 < image_ids.size()) {
        image_pairs.emplace_back(image_id1, image_ids.at(image_idx2));
        if (options_.quadratic_overlap) {
          const size_t image_idx2_quadratic = image_idx1 + (1 << i);
          if (image_idx2_quadratic < image_ids.size()) {
            image_pairs.emplace_back(image_id1,
                                     image_ids.at(image_idx2_quadratic));
          }
        }
      } else {
        break;
      }
    }

    DatabaseTransaction database_transaction(&database_);
    matcher_.Match(image_pairs);

    PrintElapsedTime(timer);
  }
}

SpatialFeatureMatcher::SpatialFeatureMatcher(
    const SpatialMatchingOptions& options,
    const SiftMatchingOptions& match_options, const std::string& database_path)
    : options_(options),
      match_options_(match_options),
      database_(database_path),
      cache_(5 * options_.max_num_neighbors, &database_),
      matcher_(match_options, &database_, &cache_) {
  CHECK(options_.Check());
  CHECK(match_options_.Check());
}

void SpatialFeatureMatcher::Run() {
  PrintHeading1("Spatial feature matching");

  if (!matcher_.Setup()) {
    return;
  }

  cache_.Setup();

  const std::vector<image_t> image_ids = cache_.GetImageIds();

  //////////////////////////////////////////////////////////////////////////////
  // Spatial indexing
  //////////////////////////////////////////////////////////////////////////////

  Timer timer;
  timer.Start();

  std::cout << "Indexing images..." << std::flush;

  GPSTransform gps_transform;

  size_t num_locations = 0;
  Eigen::Matrix<float, Eigen::Dynamic, 3, Eigen::RowMajor> location_matrix(
      image_ids.size(), 3);

  std::vector<size_t> location_idxs;
  location_idxs.reserve(image_ids.size());

  std::vector<Eigen::Vector3d> ells(1);

  for (size_t i = 0; i < image_ids.size(); ++i) {
    const auto image_id = image_ids[i];
    const auto& image = cache_.GetImage(image_id);

    if ((image.TvecPrior(0) == 0 && image.TvecPrior(1) == 0 &&
         options_.ignore_z) ||
        (image.TvecPrior(0) == 0 && image.TvecPrior(1) == 0 &&
         image.TvecPrior(2) == 0 && !options_.ignore_z)) {
      continue;
    }

    location_idxs.push_back(i);

    if (options_.is_gps) {
      ells[0](0) = image.TvecPrior(0);
      ells[0](1) = image.TvecPrior(1);
      ells[0](2) = options_.ignore_z ? 0 : image.TvecPrior(2);

      const auto xyzs = gps_transform.EllToXYZ(ells);

      location_matrix(num_locations, 0) = static_cast<float>(xyzs[0](0));
      location_matrix(num_locations, 1) = static_cast<float>(xyzs[0](1));
      location_matrix(num_locations, 2) = static_cast<float>(xyzs[0](2));
    } else {
      location_matrix(num_locations, 0) =
          static_cast<float>(image.TvecPrior(0));
      location_matrix(num_locations, 1) =
          static_cast<float>(image.TvecPrior(1));
      location_matrix(num_locations, 2) =
          static_cast<float>(options_.ignore_z ? 0 : image.TvecPrior(2));
    }

    num_locations += 1;
  }

  PrintElapsedTime(timer);

  if (num_locations == 0) {
    std::cout << " => No images with location data." << std::endl;
    GetTimer().PrintMinutes();
    return;
  }

  //////////////////////////////////////////////////////////////////////////////
  // Building spatial index
  //////////////////////////////////////////////////////////////////////////////

  timer.Restart();

  std::cout << "Building search index..." << std::flush;

  flann::Matrix<float> locations(location_matrix.data(), num_locations,
                                 location_matrix.cols());

  flann::LinearIndexParams index_params;
  flann::LinearIndex<flann::L2<float>> search_index(index_params);
  search_index.buildIndex(locations);

  PrintElapsedTime(timer);

  //////////////////////////////////////////////////////////////////////////////
  // Searching spatial index
  //////////////////////////////////////////////////////////////////////////////

  timer.Restart();

  std::cout << "Searching for nearest neighbors..." << std::flush;

  const int knn = std::min<int>(options_.max_num_neighbors, num_locations);

  Eigen::Matrix<size_t, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>
      index_matrix(num_locations, knn);
  flann::Matrix<size_t> indices(index_matrix.data(), num_locations, knn);

  Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>
      distance_matrix(num_locations, knn);
  flann::Matrix<float> distances(distance_matrix.data(), num_locations, knn);

  flann::SearchParams search_params(flann::FLANN_CHECKS_AUTOTUNED);
  if (match_options_.num_threads == ThreadPool::kMaxNumThreads) {
    search_params.cores = std::thread::hardware_concurrency();
  } else {
    search_params.cores = match_options_.num_threads;
  }
  if (search_params.cores <= 0) {
    search_params.cores = 1;
  }

  search_index.knnSearch(locations, indices, distances, knn, search_params);

  PrintElapsedTime(timer);

  //////////////////////////////////////////////////////////////////////////////
  // Matching
  //////////////////////////////////////////////////////////////////////////////

  const float max_distance =
      static_cast<float>(options_.max_distance * options_.max_distance);

  std::vector<std::pair<image_t, image_t>> image_pairs;
  image_pairs.reserve(knn);

  for (size_t i = 0; i < num_locations; ++i) {
    if (IsStopped()) {
      GetTimer().PrintMinutes();
      return;
    }

    timer.Restart();

    std::cout << StringPrintf("Matching image [%d/%d]", i + 1, num_locations)
              << std::flush;

    image_pairs.clear();

    for (int j = 0; j < knn; ++j) {
      // Check if query equals result.
      if (index_matrix(i, j) == i) {
        continue;
      }

      // Since the nearest neighbors are sorted by distance, we can break.
      if (distance_matrix(i, j) > max_distance) {
        break;
      }

      const size_t idx = location_idxs[i];
      const image_t image_id = image_ids.at(idx);
      const size_t nn_idx = location_idxs.at(index_matrix(i, j));
      const image_t nn_image_id = image_ids.at(nn_idx);
      image_pairs.emplace_back(image_id, nn_image_id);
    }

    DatabaseTransaction database_transaction(&database_);
    matcher_.Match(image_pairs);

    PrintElapsedTime(timer);
  }

  GetTimer().PrintMinutes();
}

TransitiveFeatureMatcher::TransitiveFeatureMatcher(
    const TransitiveMatchingOptions& options,
    const SiftMatchingOptions& match_options, const std::string& database_path)
    : options_(options),
      match_options_(match_options),
      database_(database_path),
      cache_(options_.batch_size, &database_),
      matcher_(match_options, &database_, &cache_) {
  CHECK(options_.Check());
  CHECK(match_options_.Check());
}

void TransitiveFeatureMatcher::Run() {
  PrintHeading1("Transitive feature matching");

  if (!matcher_.Setup()) {
    return;
  }

  cache_.Setup();

  const std::vector<image_t> image_ids = cache_.GetImageIds();

  std::vector<std::pair<image_t, image_t>> image_pairs;
  std::unordered_set<image_pair_t> image_pair_ids;

  for (int iteration = 0; iteration < options_.num_iterations; ++iteration) {
    if (IsStopped()) {
      GetTimer().PrintMinutes();
      return;
    }

    Timer timer;
    timer.Start();

    std::cout << StringPrintf("Iteration [%d/%d]", iteration + 1,
                              options_.num_iterations)
              << std::endl;

    std::vector<std::pair<image_t, image_t>> existing_image_pairs;
    std::vector<int> existing_num_inliers;
    database_.ReadNumMatches(&existing_image_pairs, &existing_num_inliers);

    CHECK_EQ(existing_image_pairs.size(), existing_num_inliers.size());

    std::unordered_map<image_t, std::vector<image_t>> adjacency;
    for (const auto& image_pair : existing_image_pairs) {
      adjacency[image_pair.first].push_back(image_pair.second);
      adjacency[image_pair.second].push_back(image_pair.first);
    }

    const size_t batch_size = static_cast<size_t>(options_.batch_size);

    size_t num_batches = 0;
    image_pairs.clear();
    image_pair_ids.clear();
    for (const auto& image : adjacency) {
      const auto image_id1 = image.first;
      for (const auto& image_id2 : image.second) {
        if (adjacency.count(image_id2) > 0) {
          for (const auto& image_id3 : adjacency.at(image_id2)) {
            const auto image_pair_id =
                Database::ImagePairToPairId(image_id1, image_id3);
            if (image_pair_ids.count(image_pair_id) == 0) {
              image_pairs.emplace_back(image_id1, image_id3);
              image_pair_ids.insert(image_pair_id);
              if (image_pairs.size() >= batch_size) {
                num_batches += 1;
                std::cout << StringPrintf("  Batch %d", num_batches)
                          << std::flush;
                DatabaseTransaction database_transaction(&database_);
                matcher_.Match(image_pairs);
                image_pairs.clear();
                PrintElapsedTime(timer);
                timer.Restart();

                if (IsStopped()) {
                  GetTimer().PrintMinutes();
                  return;
                }
              }
            }
          }
        }
      }
    }

    num_batches += 1;
    std::cout << StringPrintf("  Batch %d", num_batches) << std::flush;
    DatabaseTransaction database_transaction(&database_);
    matcher_.Match(image_pairs);
    PrintElapsedTime(timer);
  }

  GetTimer().PrintMinutes();
}

ImagePairsFeatureMatcher::ImagePairsFeatureMatcher(
    const ImagePairsMatchingOptions& options,
    const SiftMatchingOptions& match_options, const std::string& database_path)
    : options_(options),
      match_options_(match_options),
      database_(database_path),
      cache_(options.block_size, &database_),
      matcher_(match_options, &database_, &cache_) {
  CHECK(options_.Check());
  CHECK(match_options_.Check());
}

void ImagePairsFeatureMatcher::Run() {
  PrintHeading1("Custom feature matching");

  if (!matcher_.Setup()) {
    return;
  }

  cache_.Setup();

  //////////////////////////////////////////////////////////////////////////////
  // Reading image pairs list
  //////////////////////////////////////////////////////////////////////////////

  std::unordered_map<std::string, image_t> image_name_to_image_id;
  image_name_to_image_id.reserve(cache_.GetImageIds().size());
  for (const auto image_id : cache_.GetImageIds()) {
    const auto& image = cache_.GetImage(image_id);
    image_name_to_image_id.emplace(image.Name(), image_id);
  }

  std::ifstream file(options_.match_list_path);
  CHECK(file.is_open()) << options_.match_list_path;

  std::string line;
  std::vector<std::pair<image_t, image_t>> image_pairs;
  while (std::getline(file, line)) {
    StringTrim(&line);

    if (line.empty() || line[0] == '#') {
      continue;
    }

    std::stringstream line_stream(line);

    std::string image_name1;
    std::string image_name2;

    std::getline(line_stream, image_name1, ' ');
    StringTrim(&image_name1);
    std::getline(line_stream, image_name2, ' ');
    StringTrim(&image_name2);

    if (image_name_to_image_id.count(image_name1) == 0) {
      std::cerr << "ERROR: Image " << image_name1 << " does not exist."
                << std::endl;
      continue;
    }
    if (image_name_to_image_id.count(image_name2) == 0) {
      std::cerr << "ERROR: Image " << image_name2 << " does not exist."
                << std::endl;
      continue;
    }

    image_pairs.emplace_back(image_name_to_image_id.at(image_name1),
                             image_name_to_image_id.at(image_name2));
  }

  //////////////////////////////////////////////////////////////////////////////
  // Feature matching
  //////////////////////////////////////////////////////////////////////////////

  const size_t num_match_blocks = image_pairs.size() / options_.block_size + 1;
  std::vector<std::pair<image_t, image_t>> block_image_pairs;
  block_image_pairs.reserve(options_.block_size);

  for (size_t i = 0; i < image_pairs.size(); i += options_.block_size) {
    if (IsStopped()) {
      GetTimer().PrintMinutes();
      return;
    }

    Timer timer;
    timer.Start();

    std::cout << StringPrintf("Matching block [%d/%d]",
                              i / options_.block_size + 1, num_match_blocks)
              << std::flush;

    const size_t block_end = i + options_.block_size <= image_pairs.size()
                                 ? i + options_.block_size
                                 : image_pairs.size();
    std::vector<std::pair<image_t, image_t>> block_image_pairs;
    block_image_pairs.reserve(options_.block_size);
    for (size_t j = i; j < block_end; ++j) {
      block_image_pairs.push_back(image_pairs[j]);
    }

    DatabaseTransaction database_transaction(&database_);
    matcher_.Match(block_image_pairs);

    PrintElapsedTime(timer);
  }

  GetTimer().PrintMinutes();
}

FeaturePairsFeatureMatcher::FeaturePairsFeatureMatcher(
    const FeaturePairsMatchingOptions& options,
    const SiftMatchingOptions& match_options, const std::string& database_path)
    : options_(options),
      match_options_(match_options),
      database_(database_path),
      cache_(kCacheSize, &database_) {
  CHECK(options_.Check());
  CHECK(match_options_.Check());
}

void FeaturePairsFeatureMatcher::Run() {
  PrintHeading1("Importing matches");

  cache_.Setup();

  std::unordered_map<std::string, const Image*> image_name_to_image;
  image_name_to_image.reserve(cache_.GetImageIds().size());
  for (const auto image_id : cache_.GetImageIds()) {
    const auto& image = cache_.GetImage(image_id);
    image_name_to_image.emplace(image.Name(), &image);
  }

  std::ifstream file(options_.match_list_path);
  CHECK(file.is_open()) << options_.match_list_path;

  std::string line;
  while (std::getline(file, line)) {
    if (IsStopped()) {
      GetTimer().PrintMinutes();
      return;
    }

    StringTrim(&line);
    if (line.empty()) {
      continue;
    }

    std::istringstream line_stream(line);

    std::string image_name1, image_name2;
    try {
      line_stream >> image_name1 >> image_name2;
    } catch (...) {
      std::cerr << "ERROR: Could not read image pair." << std::endl;
      break;
    }

    std::cout << StringPrintf("%s - %s", image_name1.c_str(),
                              image_name2.c_str())
              << std::endl;

    if (image_name_to_image.count(image_name1) == 0) {
      std::cout << StringPrintf("SKIP: Image %s not found in database.",
                                image_name1.c_str())
                << std::endl;
      break;
    }
    if (image_name_to_image.count(image_name2) == 0) {
      std::cout << StringPrintf("SKIP: Image %s not found in database.",
                                image_name2.c_str())
                << std::endl;
      break;
    }

    const Image& image1 = *image_name_to_image[image_name1];
    const Image& image2 = *image_name_to_image[image_name2];

    bool skip_pair = false;
    if (database_.ExistsMatches(image1.ImageId(), image2.ImageId())) {
      std::cout << "SKIP: Matches for image pair already exist in database."
                << std::endl;
      skip_pair = true;
    }

    FeatureMatches matches;
    while (std::getline(file, line)) {
      StringTrim(&line);

      if (line.empty()) {
        break;
      }

      std::istringstream line_stream(line);

      FeatureMatch match;
      try {
        line_stream >> match.line_idx1 >> match.line_idx2;
      } catch (...) {
        std::cerr << "ERROR: Cannot read feature matches." << std::endl;
        break;
      }

      matches.push_back(match);
    }

    if (skip_pair) {
      continue;
    }

    database_.WriteMatches(image1.ImageId(), image2.ImageId(), matches);
  }

  GetTimer().PrintMinutes();
}

}  // namespace colmap
