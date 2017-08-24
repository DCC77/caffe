#ifndef CAFFE_CUDNN_CONV_LAYER_HPP_
#define CAFFE_CUDNN_CONV_LAYER_HPP_

#include <vector>

#include "caffe/blob.hpp"
#include "caffe/layer.hpp"

#include "caffe/layers/conv_layer.hpp"

#ifndef CPU_ONLY

#include "caffe/util/gpu_memory.hpp"

#endif

namespace caffe {

#ifdef USE_CUDNN

#if CUDNN_VERSION_MIN(7, 0, 0)
  #define CUDNN_GROUPING
#endif

/*
 * @brief cuDNN implementation of ConvolutionLayer.
 *        Fallback to ConvolutionLayer for CPU mode.
 *
 * cuDNN accelerates convolution through forward kernels for filtering and bias
 * plus backward kernels for the gradient w.r.t. the filters, biases, and
 * inputs. Caffe + cuDNN further speeds up the computation through forward
 * parallelism across groups and backward parallelism across gradients.
 *
 * The CUDNN engine does not have memory overhead for matrix buffers. For many
 * input and filter regimes the CUDNN engine is faster than the CAFFE engine,
 * but for fully-convolutional models and large inputs the CAFFE engine can be
 * faster as long as it fits in memory.
*/
template<typename Ftype, typename Btype>
class CuDNNConvolutionLayer : public ConvolutionLayer<Ftype, Btype> {
  // In iteration 0, use a small amount of memory in order to leave
  // most of memory for allocating layer blobs.
  // NOLINT_NEXT_LINE(build/storage_class)
  static constexpr size_t INITIAL_WORKSPACE_SIZE = 8 * 1024 * 1024;
  // Using all of memory may result in failure of workspace reserve.
  // NOLINT_NEXT_LINE(build/storage_class)
  static constexpr size_t PAGE_SIZE = 16 * 1024 * 1024;
  // We update it on second Fwd/Bwd pass and we allocate it *once*
  // when we start third pass. We might recompute it later if demand grows
  // and/or we suddenly have to get extra memory for other needs.
  static ThreadSafeMap<std::map<int, size_t>> mem_size_estimated_;
  static size_t& mem_size_estimated(int device) {
    return mem_size_estimated_[device];
  }
  static ThreadSafeMap<std::map<int, size_t>> mem_req_all_grps_;
  static size_t& mem_req_all_grps(int device) {
    return mem_req_all_grps_[device];
  }
  // Workspace used by all Convolution layers one after another.
  // We carry it global to prevent unnecessary allocations/deallocations
  // because they hurt performance.
  static ThreadSafeMap<std::map<int, shared_ptr<GPUMemory::Workspace>>> workspace_;
  static GPUMemory::Workspace& workspace(int device) {
    return *workspace_[device];
  }
  static ThreadSafeMap<std::map<int, shared_ptr<GPUMemory::Workspace>>> tmp_weights_;
  static GPUMemory::Workspace& tmp_weights(int device) {
    return *tmp_weights_[device];
  }
  // Stop alloc/dealloc
  static ThreadSafeMap<std::map<int, bool>> was_reduced_;
  static bool& was_reduced(int device) {
    return was_reduced_[device];
  }

 public:
  explicit CuDNNConvolutionLayer(const LayerParameter& param)
      : ConvolutionLayer<Ftype, Btype>(param), handles_setup_(false),
        use_algo_seeker_(true), use_modest_workspace_(true),
        forward_math_(tpmax<Ftype, float>()), backward_data_math_(tpmax<Btype, float>()),
        backward_filter_math_(tpmax<Btype, float>()) {
#if CUDNN_VERSION_MIN(7, 0, 0)
    cudnn_math_override_ = -1;
#endif
  }

  virtual void LayerSetUp(const vector<Blob*>& bottom, const vector<Blob*>& top);
  virtual void Reshape(const vector<Blob*>& bottom, const vector<Blob*>& top);
  virtual ~CuDNNConvolutionLayer();

 protected:
  virtual void Forward_gpu(const vector<Blob*>& bottom, const vector<Blob*>& top);
  virtual void Backward_gpu(const vector<Blob*>& top, const vector<bool>& propagate_down,
      const vector<Blob*>& bottom);

  bool handles_setup_;

  // algorithms for forward and backwards convolutions
  vector<cudnnConvolutionFwdAlgo_t> fwd_algo_;
  vector<cudnnConvolutionBwdFilterAlgo_t> bwd_filter_algo_;
  vector<cudnnConvolutionBwdDataAlgo_t> bwd_data_algo_;

#if CUDNN_VERSION_MIN(7, 0, 0)
  int cudnn_math_override_;
  vector<cudnnMathType_t> fwd_cudnn_math_, bwd_filter_cudnn_math_, bwd_data_cudnn_math_;
#endif

  vector<cudnnTensorDescriptor_t> fwd_bottom_descs_, fwd_top_descs_;
  vector<cudnnTensorDescriptor_t> bwd_bottom_descs_, bwd_top_descs_;
  cudnnTensorDescriptor_t fwd_bias_desc_, bwd_bias_desc_;
  cudnnFilterDescriptor_t fwd_filter_desc_, bwd_filter_desc_;
  vector<cudnnConvolutionDescriptor_t> fwd_conv_descs_;
  vector<cudnnConvolutionDescriptor_t> bwd_conv_data_descs_, bwd_conv_filter_descs_;

  int bottom_offset_, top_offset_, bias_offset_;

  vector<size_t> workspace_fwd_sizes_;
  vector<size_t> workspace_bwd_data_sizes_;
  vector<size_t> workspace_bwd_filter_sizes_;

 private:
  // while very 1st cycle: true/true
  // while second cycle:   true/false
  // while third+ cycle:   false/false
  bool use_algo_seeker_;
  bool use_modest_workspace_;

  vector<int> user_algos_override_;

  void FindExConvAlgo(const vector<Blob*>& bottom, const vector<Blob*>& top);
  void EstimateMaxWorkspaceSize(const vector<Blob*>& bottom, const vector<Blob*>& top);
  void GetConvAlgo(const vector<Blob*>& bottom, const vector<Blob*>& top,
      const size_t workspace_bytes, int pad_h, int pad_w, int stride_h, int stride_w);

  size_t ComputeFindExWorkspaceSize();

  vector<cudnnTensorDescriptor_t> fwd_cached_bottom_descs_, bwd_cached_bottom_descs_;
  vector<cudnnConvolutionDescriptor_t> fwd_cached_conv_descs_,
      bwd_cached_conv_data_descs_, bwd_cached_conv_filter_descs_;
  bool IsBottomDescChanged(const vector<Blob*>& bottom, bool fwd_mode);
  bool IsConvDescChanged(const vector<Blob*>& bottom, bool fwd_mode);

  bool use_reshape_;
  bool initialized_cached_descs_;
  static constexpr int MAX_PARALLEL_GROUPS = 1;//2;
  static constexpr int REQUEST_ALGO_COUNT = 1;
  static constexpr int ATTEMPTS_TO_RESERVE_WS = 3;

  bool use_v7grouping() const {
#ifdef CUDNN_GROUPING
    return this->channels_ == this->num_output_ && this->channels_ == this->group_;
#else
    return false;
#endif
  }

  int groups() {
    return use_v7grouping() ? this->group_ : std::min(this->group_, MAX_PARALLEL_GROUPS);
  }

  int ws_groups() {
    return use_v7grouping() ? 1 : this->group_;//std::min(this->group_, MAX_PARALLEL_GROUPS);
  }

  int agr_groups() {
    return this->group_;
  }

  int idxg(int group) {
    return group % MAX_PARALLEL_GROUPS;
  }

  // This is current *demand*: it might be not yet allocated.
  void UpdateWorkspaceDemand(int size);

  Type forward_math_, backward_data_math_, backward_filter_math_;
};

template<typename Ftype, typename Btype>
constexpr size_t CuDNNConvolutionLayer<Ftype, Btype>::INITIAL_WORKSPACE_SIZE;

template<typename Ftype, typename Btype>
constexpr size_t CuDNNConvolutionLayer<Ftype, Btype>::PAGE_SIZE;

template<typename Ftype, typename Btype>
constexpr int CuDNNConvolutionLayer<Ftype, Btype>::MAX_PARALLEL_GROUPS;

template<typename Ftype, typename Btype>
constexpr int CuDNNConvolutionLayer<Ftype, Btype>::REQUEST_ALGO_COUNT;

template<typename Ftype, typename Btype>
constexpr int CuDNNConvolutionLayer<Ftype, Btype>::ATTEMPTS_TO_RESERVE_WS;

template<typename Ftype, typename Btype>
ThreadSafeMap<std::map<int, shared_ptr<GPUMemory::Workspace>>>
    CuDNNConvolutionLayer<Ftype, Btype>::workspace_;

template<typename Ftype, typename Btype>
ThreadSafeMap<std::map<int, shared_ptr<GPUMemory::Workspace>>>
    CuDNNConvolutionLayer<Ftype, Btype>::tmp_weights_;

template<typename Ftype, typename Btype>
ThreadSafeMap<std::map<int, size_t>>
    CuDNNConvolutionLayer<Ftype, Btype>::mem_size_estimated_;

template<typename Ftype, typename Btype>
ThreadSafeMap<std::map<int, size_t>>
    CuDNNConvolutionLayer<Ftype, Btype>::mem_req_all_grps_;

template<typename Ftype, typename Btype>
ThreadSafeMap<std::map<int, bool>>
    CuDNNConvolutionLayer<Ftype, Btype>::was_reduced_;

#endif

}  // namespace caffe

#endif  // CAFFE_CUDNN_CONV_LAYER_HPP_
