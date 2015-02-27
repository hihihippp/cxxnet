#include <utility>
#include <vector>
#include <string>
#include <cstring>
#include <cstdlib>
#include "./nnet.h"
#include "../utils/io.h"
#include "../utils/metric.h"
#include "./neural_net-inl.hpp"


namespace cxxnet {
namespace nnet {
/*! \brief implementation of neural network trainer, using multiple threads */
template<typename xpu>
class CXXNetThreadTrainer : public INetTrainer {
 public:
  CXXNetThreadTrainer(void) {
    batch_size = 100;
    update_period = 1;
    sample_counter = 0;
    eval_train = 1;
    epoch_counter = 0;
    seed = 0;
    pserver = NULL;
    type_pserver = "UNSPECIFIED";
  }
  virtual ~CXXNetThreadTrainer(void) {
    this->FreeNet();
  }
  virtual void SetParam(const char *name, const char *val) {
    if (!strcmp(name, "dev")) {
      devices_.clear();
      const char *devs = strchr(val, ':');
      if (devs != NULL) {
        int a, b;
        if (sscanf(devs + 1, "%d-%d", &a, &b) == 2) {
          for (int i = a; i <= b; ++i) {
            devices_.push_back(i);
          }
        } else {
          std::string s_dev = devs + 1;
          char *ptr = strtok(&s_dev[0], ",");
          while (ptr != NULL) {
            utils::Check(sscanf(ptr, "%d", &a) == 1, "invalid device format");
            devices_.push_back(a);
            ptr = strtok(NULL, ",");
          }
        }
      }
    }
    if (!strcmp(name, "batch_size")) batch_size = static_cast<mshadow::index_t>(atoi(val));
    if (!strcmp(name, "update_period")) update_period = atoi(val);
    if (!strcmp(name, "eval_train")) eval_train = atoi(val);
    if (!strcmp(name, "seed")) seed = atoi(val);
    if (!strcmp(name, "param_server")) type_pserver = val;
    if (!strncmp(name, "metric", 6)) {
      char label_name[256];
      if (sscanf(name, "metric[%[^]]]", label_name) == 1){
        metric.AddMetric(val, label_name); train_metric.AddMetric(val, label_name);
      } else {
        metric.AddMetric(val, "label"); train_metric.AddMetric(val, "label");
      }
    }
    cfg.push_back(std::make_pair(std::string(name), std::string(val)));
  }
  virtual void InitModel(void) {
    this->InitNet();
    nets_[0]->InitModel();
    nets_[0]->WaitJob();
    this->Save2ModelBlob();
    for(size_t i = 1; i < nets_.size(); ++i) {
      utils::MemoryBufferStream fs(&model_blob_);
      nets_[i]->LoadModel(fs);
      nets_[i]->WaitJob();
    }
    this->InitTemp();
  }
  virtual void SaveModel(utils::IStream &fo) {
    this->Save2ModelBlob();
    net_cfg.SaveNet(fo);
    fo.Write(&epoch_counter, sizeof(epoch_counter));
    fo.Write(model_blob_);
  }
  virtual void LoadModel(utils::IStream &fi) {
    net_cfg.LoadNet(fi);
    fi.Read(&epoch_counter, sizeof(epoch_counter));
    this->FreeNet();
    this->InitNet();
    fi.Read(&model_blob_);
    for (size_t i = 0; i < nets_.size(); ++i) {
      utils::MemoryBufferStream fs(&model_blob_);
      nets_[i]->LoadModel(fs);
      nets_[i]->WaitJob();
    }
    this->InitTemp();
  }
  virtual void CopyModelFrom(utils::IStream &fi) {
    this->FreeNet();
    this->InitModel();

    // Load the original net
    NetConfig old_cfg;
    old_cfg.LoadNet(fi);
    fi.Read(&epoch_counter, sizeof(epoch_counter));
    epoch_counter = 0;
    NeuralNet<cpu> old_net(old_cfg, 0, 0, NULL);
    std::string old_model;
    fi.Read(&old_model);
    utils::MemoryBufferStream os(&old_model);
    old_net.LoadModel(os);

    // Compare original net and current net
    for (index_t i = 0; i < old_cfg.layers.size(); ++i){
      std::string& old_name = old_cfg.layers[i].name;
      for (index_t j = 0; j < net_cfg.layers.size(); ++j){
        std::string& new_name = net_cfg.layers[j].name;
        if (old_name == new_name && old_name != ""){
          printf("Copying layer %s\n", old_name.c_str());
          std::string data;
          utils::MemoryBufferStream fs(&data);
          old_net.connections[i].layer->SaveModel(fs);
          for (index_t k = 0; k < nets_.size(); ++k){
            fs.Seek(0);
            nets_[k]->CopyLayer(j, fs);
            nets_[k]->WaitJob();
          }
        }
      }
    }
  }
  virtual void StartRound(int round) {
    for (size_t i = 0; i < nets_.size(); ++i) {
      nets_[i]->StartRound(round);
    }
    this->WaitAllJobs();
  }
  virtual void Update(const DataBatch& data) {
    mshadow::Shape<4> oshape = out_temp.shape_;
    oshape[0] = data.batch_size;
    out_temp.Resize(oshape);

    const size_t ndevice = devices_.size();
    mshadow::index_t step = std::max((batch_size + ndevice - 1) / ndevice, 1UL);

    bool need_sync = sample_counter % update_period == 0;
    bool need_update = (sample_counter + 1) % update_period == 0;
    layer::LabelInfo info = GetLabelInfo(data);

    for (mshadow::index_t i = nets_.size(); i != 0; --i) {
      mshadow::index_t begin = std::min((i - 1) * step, data.batch_size);
      mshadow::index_t end = std::min(i * step, data.batch_size);
      std::vector<mshadow::Tensor<mshadow::cpu, 4> > extra_data;
      for (mshadow::index_t j = 0; j < data.extra_data.size(); ++j){
        extra_data.push_back(data.extra_data[j].Slice(begin, end));
      }
      nets_[i - 1]->TrainForwardBackprop(data.data.Slice(begin, end),
                                         extra_data,
                                         info.Slice(begin, end),
                                         out_temp.Slice(begin, end),
                                         false, need_sync,
                                         need_update, epoch_counter);
    }
    this->WaitAllJobs();
    // evlauate training loss
    if (eval_train != 0) {
      train_metric.AddEval(out_temp.FlatTo2D(), info);
    }
    if (++sample_counter >= update_period) {
      sample_counter = 0;
      epoch_counter += 1;
    }
  }
  virtual void Predict(mshadow::TensorContainer<mshadow::cpu, 1> *out_preds,
                       const DataBatch &data) {
    mshadow::TensorContainer<mshadow::cpu, 1> &preds = *out_preds;
    this->ForwardTo(&out_temp, data, nets_[0]->net().nodes.size() - 1);
    preds.Resize(mshadow::Shape1(out_temp.size(0)));
    for (index_t i = 0; i < out_temp.size(0); ++i) {
      preds[i] = this->TransformPred(out_temp[i][0][0]);
    }
  }
  virtual void ExtractFeature(mshadow::TensorContainer<mshadow::cpu, 4> *out_preds,
                              const DataBatch &batch,
                              const char *node_name_) {
    std::string node_name = node_name_;
    std::map<std::string, int> &name_map = net_cfg.node_name_map;
    int node_id, offset;
    if (sscanf(node_name.c_str(), "top[-%d]", &offset) == 1) {
      int nnode = static_cast<int>(nets_[0]->net().nodes.size());
      utils::Check(offset >= 1 && offset <= nnode,
                   "ExtractFeature: offset must be within num_node range");
      node_id = nnode - offset;
    } else {
      utils::Check(name_map.find(node_name) != name_map.end(),
                   "ExtractFeature: Cannot find node name: %s", node_name.c_str());
      node_id = name_map[node_name];
    }
    this->ForwardTo(out_preds, batch, node_id);
  }
  virtual std::string Evaluate(IIterator<DataBatch> *iter_eval, const char *data_name) {
    std::string ret;
    if (eval_train != 0) {
      ret += train_metric.Print("train");
      train_metric.Clear();
    }
    if (iter_eval == NULL) return ret;
    metric.Clear();
    iter_eval->BeforeFirst();
    while (iter_eval->Next()) {
      const DataBatch& batch = iter_eval->Value();
      this->ForwardTo(&out_temp, batch, nets_[0]->net().nodes.size() - 1);
      metric.AddEval(out_temp.Slice(0, out_temp.size(0) - batch.num_batch_padd).FlatTo2D(),
        GetLabelInfo(batch));
    }
    ret += metric.Print(data_name);
    return ret;
  }
  virtual void SetWeight(mshadow::Tensor<mshadow::cpu, 2> weight,
                         const char *layer_name,
                         const char *weight_tag) {
    utils::Check(!strcmp(weight_tag, "bias") ||
                 !strcmp(weight_tag, "wmat"),
                 "NNet.SetWeight: weight tag can only be bias or wmat");
    int layer_index = net_cfg.GetLayerIndex(layer_name);
    for (size_t i = 0; i < nets_.size(); ++i) {
      nets_[i]->SetWeight(layer_index, weight, weight_tag);
    }
    this->WaitAllJobs();
  }
  virtual void GetWeight(mshadow::TensorContainer<mshadow::cpu, 2> *out_weight,
                         std::vector<index_t> *out_shape,
                         const char *layer_name,
                         const char *weight_tag) {
    utils::Check(!strcmp(weight_tag, "bias") ||
                 !strcmp(weight_tag, "wmat"),
                 "NNet.GetWeight: weight tag can only be bias or wmat");
    int layer_index = net_cfg.GetLayerIndex(layer_name);
    nets_[0]->GetWeight(layer_index, out_weight, out_shape, weight_tag);
    nets_[0]->WaitJob();
  }

 private:
  inline layer::LabelInfo GetLabelInfo(const DataBatch &data) const {
    layer::LabelInfo info;
    layer::LabelRecord rec;
    info.name2findex = &net_cfg.label_name_map;
    for (size_t i = 0; i < net_cfg.label_range.size(); ++i) {
      index_t begin =  net_cfg.label_range[i].first;
      index_t end =  net_cfg.label_range[i].second;
      rec.label = mshadow::Tensor<cpu, 2>
          (data.label.dptr_ + begin,
           mshadow::Shape2(data.batch_size, end - begin),
           data.label.stride_, NULL);
      info.fields.push_back(rec);
    }
    return info;
  }
  inline float TransformPred(mshadow::Tensor<cpu,1> pred) {
    if (pred.size(0) != 1) {
      return GetMaxIndex(pred);
    } else {
      return pred[0];
    }
  }
  inline static int GetMaxIndex(mshadow::Tensor<cpu,1> pred) {
    index_t maxidx = 0;
    for(index_t i = 1; i < pred.size(0); ++ i) {
      if(pred[i] > pred[maxidx]) maxidx = i;
    }
    return maxidx;
  }
  inline void ForwardTo(mshadow::TensorContainer<mshadow::cpu, 4> *out_data,
                        const DataBatch &data,
                        int layer) {
    mshadow::Shape<4> oshape = nets_[0]->net().nodes[layer].data.shape_;
    oshape[0] = data.batch_size;
    out_data->Resize(oshape);
    const size_t ndevice = devices_.size();
    mshadow::index_t step = std::max((batch_size + ndevice - 1) / ndevice, 1UL);
    for (mshadow::index_t i = nets_.size(); i != 0; --i) {
      mshadow::index_t begin = std::min((i - 1) * step, data.batch_size);
      mshadow::index_t end = std::min(i * step, data.batch_size);
      mshadow::Tensor<cpu, 4> mbatch = data.data.Slice(begin, end);
      std::vector<mshadow::Tensor<mshadow::cpu, 4> > extra_data;
      for (mshadow::index_t j = 0; j < data.extra_data.size(); ++j){
        extra_data.push_back(data.extra_data[j].Slice(begin, end));
      }
      nets_[i - 1]->PredictForward(mbatch, extra_data);
    }
    this->WaitAllJobs();
    // copy results out
    for (mshadow::index_t i = nets_.size(); i != 0; --i) {
      mshadow::index_t begin = std::min((i - 1) * step, data.batch_size);
      mshadow::index_t end = std::min(i * step, data.batch_size);
      nets_[i - 1]->CopyNodeData(layer, out_data->Slice(begin, end));
    }
    this->WaitAllJobs();
  }

  inline void WaitAllJobs(void) {
    for (size_t i = nets_.size(); i != 0; --i) {
      nets_[i - 1]->WaitJob();
    }
  }
  inline void Save2ModelBlob(void) {
    // save to model blob
    model_blob_.clear();
    utils::MemoryBufferStream fs(&model_blob_);
    nets_[0]->SaveModel(fs);
    nets_[0]->WaitJob();
  }
  inline void InitNet(void) {
    utils::Assert(nets_.size() == 0, "net must be empty before this");
    net_cfg.Configure(cfg);
    if (devices_.size() == 0) devices_.push_back(0);
    size_t ndevice = devices_.size();
    mshadow::index_t step = std::max((batch_size + ndevice - 1) / ndevice, 1UL);
    while (step * (devices_.size() - 1) >= batch_size) {
      devices_.pop_back();
    }
    if (ndevice > devices_.size()) {
      ndevice = devices_.size();
      if (silent == 0) {
        printf("Warning: The number of devices is induce mini-batch=%u\n" \
               "We can equally use %lu devices to cover the batch_size\n", step, ndevice);
      }
    }
    this->InitParamServer();
    for (size_t i = 0; i < ndevice; ++i) {
      nets_.push_back(new NeuralNetThread<xpu>(net_cfg, pserver,
                                               devices_[i], step, i + seed * 100));
    }
    if (silent == 0) {
      printf("finish initialization with %lu devices\n", devices_.size());
    }
  }
  inline void InitParamServer(void) {
    utils::Assert(pserver == NULL, "net must be empty before this");
    if (type_pserver == "UNSPECIFIED") {
      if (devices_.size() <=1) type_pserver = "NONE";
      else type_pserver = "local";
    }
    if (type_pserver != "NONE") {
      pserver = mshadow::ps::CreateSharedModel<xpu, real_t>(type_pserver.c_str());
      for (size_t i = 0; i < cfg.size(); ++i) {
        pserver->SetParam(cfg[i].first.c_str(), cfg[i].second.c_str());
      }
      if (devices_.size() == 0) devices_.push_back(0);
      pserver->Init(devices_);
    }
  }
  inline void InitTemp(void) {
    mshadow::Shape<4> oshape = nets_[0]->net().nodes.back().data.shape_;
    oshape[0] = batch_size;
    out_temp.Resize(oshape);
  }
  inline void FreeNet(void) {
    for (size_t i = 0; i < nets_.size(); ++i) {
      delete nets_[i];
    }
    nets_.clear();
    if (pserver != NULL) {
      delete pserver;
      pserver = NULL;
    }
  }
  /*! \brief parameter server */
  mshadow::ps::ISharedModel<xpu, real_t> *pserver;
  /*! \brief type of parameter server */
  std::string type_pserver;
  /*! \brief epoch counter */
  long epoch_counter;
  /*! \brief seed to the layers */
  int seed;
  /*! \brief silent*/
  int silent;
  /*! \brief update period */
  int update_period;
  /*! \brief sample counter */
  int sample_counter;
  /*! \brief show train eval */
  int eval_train;
  /*! \brief evaluator */
  utils::MetricSet metric;
  /*! \brief evaluator for train */
  utils::MetricSet train_metric;
  /*! \brief final space of output node */
  mshadow::TensorContainer<cpu, 4> out_temp;
  // ------- model part --------
  /*! \brief batch size */
  mshadow::index_t batch_size;
  /*! \brief record the devices_ used in each thread */
  std::vector<int> devices_;
  /*! \brief serialized model in CPU */
  std::string model_blob_;
  /*! \brief threads of neural nets */
  std::vector<NeuralNetThread<xpu>*> nets_;

  /*! \brief network configuration type */
  NetConfig net_cfg;
  /*! \brief history of configurations */
  std::vector< std::pair<std::string, std::string> > cfg;
};

template<typename xpu>
INetTrainer *CreateNet_(int net_type) {
  return new CXXNetThreadTrainer<xpu>();
}
}  // namespace nnet
}  // cxxnet
