#include "vstcompare/analysis_signal.hpp"

#include <algorithm>

namespace vstcompare {

AudioBuffer toMonoForAnalysis(const AudioBuffer& in, std::string& modeOut) {
  if (in.numChannels() == 0 || in.numSamples() == 0) {
    modeOut = "single_channel";
    return AudioBuffer::zeros(1, 0, in.sampleRate);
  }

  if (in.numChannels() >= 2) {
    AudioBuffer mono = AudioBuffer::zeros(1, in.numSamples(), in.sampleRate);
    for (std::size_t i = 0; i < in.numSamples(); ++i) {
      mono.channels[0][i] = 0.5f * (in.channels[0][i] + in.channels[1][i]);
    }
    modeOut = "mono_lr_average";
    return mono;
  }

  AudioBuffer copied = AudioBuffer::zeros(1, in.numSamples(), in.sampleRate);
  std::copy(in.channels[0].begin(), in.channels[0].end(), copied.channels[0].begin());
  modeOut = "single_channel";
  return copied;
}

}  // namespace vstcompare

