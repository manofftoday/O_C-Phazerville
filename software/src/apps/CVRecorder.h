#pragma once

#include "../OC_apps.h"

static const uint8_t CVREC_HORUS_PLAY[16] = {
  0x00, 0x00, 0x18, 0x24, 0x42, 0x81, 0x81, 0x42,
  0x24, 0x18, 0x00, 0x08, 0x10, 0x20, 0x40, 0x80
};

static const uint8_t CVREC_HORUS_REC[16] = {
  0x00, 0x18, 0x3c, 0x7e, 0xff, 0xff, 0xe7, 0xff,
  0xff, 0xe7, 0xff, 0xff, 0x7e, 0x3c, 0x18, 0x08
};

OC_APP_CLASS(AppCVRecorder, TWOCCS("CR"), "CV Recorder 4", "4x CV Recorder") {
public:
  static constexpr int kMaxStep = 384;
  OC_APP_INTERFACE_DECLARE(AppCVRecorder, 8);

private:
  enum Mode : uint8_t {
    REC_1_2,
    REC_1_2_3_4,
  };

  static constexpr int kTrackCount = 4;
  static constexpr int kScopeWidth = 64;
  static constexpr int kClockPeriodDefault = 1000;

  int16_t cv_[kTrackCount][kMaxStep]{};
  int32_t output_[kTrackCount]{};
  int start_step_ = 0;
  int end_step_ = 63;
  int play_step_ = 0;
  int record_step_ = 0;
  int clock_divider_ = 0;
  uint32_t clock_period_ = kClockPeriodDefault;
  uint32_t clock_ticks_ = 0;
  Mode mode_ = REC_1_2;
  bool smooth_ = false;
  bool recording_ = false;
  mutable int16_t scope_[kTrackCount][kScopeWidth]{};
  mutable uint8_t scope_pos_ = 0;
  uint8_t scope_tick_ = 0;

  void StartRecording();
  void AdvancePlayback();
  void RecordStep(const OC::IOFrame *ioframe);
  void BypassInputs(const OC::IOFrame *ioframe);
  int ActiveTrackCount() const { return mode_ == REC_1_2 ? 2 : 4; }
  int NextStep(int step) const { return step >= end_step_ ? start_step_ : step + 1; }
  int16_t RecordedValue(int track, int step) const;
  void DrawScope() const;
};

FLASHMEM void AppCVRecorder::Init() {
  start_step_ = 0;
  end_step_ = 63;
  play_step_ = 0;
  record_step_ = 0;
  clock_divider_ = 0;
  clock_period_ = kClockPeriodDefault;
  clock_ticks_ = 0;
  mode_ = REC_1_2;
  smooth_ = false;
  recording_ = false;
  scope_pos_ = 0;
  scope_tick_ = 0;
  memset(cv_, 0, sizeof(cv_));
  memset(output_, 0, sizeof(output_));
  memset(scope_, 0, sizeof(scope_));
}

FLASHMEM size_t AppCVRecorder::SaveAppData(util::StreamBufferWriter &stream_buffer) const {
  stream_buffer.Write(static_cast<uint16_t>(start_step_));
  stream_buffer.Write(static_cast<uint16_t>(end_step_));
  stream_buffer.Write(static_cast<uint8_t>(mode_));
  stream_buffer.Write(static_cast<uint8_t>(smooth_));
  stream_buffer.Write(static_cast<uint16_t>(0));
  return stream_buffer.written();
}

FLASHMEM size_t AppCVRecorder::RestoreAppData(util::StreamBufferReader &stream_buffer) {
  uint16_t start = 0;
  uint16_t end = 63;
  uint8_t mode = REC_1_2;
  uint8_t smooth = 0;
  uint16_t reserved = 0;
  stream_buffer.Read(start);
  stream_buffer.Read(end);
  stream_buffer.Read(mode);
  stream_buffer.Read(smooth);
  stream_buffer.Read(reserved);
  start_step_ = constrain(static_cast<int>(start), 0, kMaxStep - 2);
  end_step_ = constrain(static_cast<int>(end), start_step_ + 1, kMaxStep - 1);
  mode_ = mode == REC_1_2_3_4 ? REC_1_2_3_4 : REC_1_2;
  smooth_ = smooth != 0;
  return stream_buffer.read();
}

void AppCVRecorder::HandleAppEvent(OC::AppEvent event) {
  if (event == OC::APP_EVENT_RESUME) {
    recording_ = false;
    play_step_ = start_step_;
  }
}

void AppCVRecorder::StartRecording() {
  recording_ = true;
  record_step_ = start_step_;
  clock_divider_ = 0;
}

int16_t AppCVRecorder::RecordedValue(int track, int step) const {
  return cv_[track][constrain(step, start_step_, end_step_)];
}

void AppCVRecorder::RecordStep(const OC::IOFrame *ioframe) {
  cv_[0][record_step_] = static_cast<int16_t>(ioframe->cv.pitch_values[0]);
  if ((clock_divider_ & 1) == 0)
    cv_[1][record_step_] = static_cast<int16_t>(ioframe->cv.pitch_values[1]);
  if ((clock_divider_ & 3) == 0)
    cv_[2][record_step_] = static_cast<int16_t>(ioframe->cv.pitch_values[2]);
  cv_[3][record_step_] = static_cast<int16_t>(ioframe->cv.pitch_values[3]);

  if (record_step_ >= end_step_) {
    recording_ = false;
    play_step_ = start_step_;
  } else {
    ++record_step_;
  }
}

void AppCVRecorder::BypassInputs(const OC::IOFrame *ioframe) {
  for (int track = 0; track < kTrackCount; ++track) {
    const int source = mode_ == REC_1_2 ? track % 2 : track;
    output_[track] = ioframe->cv.pitch_values[source];
  }
}

void AppCVRecorder::AdvancePlayback() {
  play_step_ = NextStep(play_step_);
  const int next_step = NextStep(play_step_);
  for (int track = 0; track < kTrackCount; ++track) {
    const int source = mode_ == REC_1_2 ? track % 2 : track;
    const int32_t current = RecordedValue(source, play_step_);
    if (!smooth_) {
      output_[track] = current;
    } else {
      const int32_t next = RecordedValue(source, next_step);
      const uint32_t period = clock_period_ ? clock_period_ : 1;
      const uint32_t phase = min(clock_ticks_, period);
      output_[track] = current + static_cast<int32_t>(
        (static_cast<int64_t>(next - current) * phase) / period);
    }
  }
}

void AppCVRecorder::Process(OC::IOFrame *ioframe) {
  ++clock_ticks_;
  if (ioframe->digital_inputs.triggered<OC::DIGITAL_INPUT_4>())
    StartRecording();

  if (ioframe->digital_inputs.triggered<OC::DIGITAL_INPUT_1>()) {
    if (clock_ticks_ > 0 && clock_ticks_ < 0xFFFFFFFFu)
      clock_period_ = clock_ticks_;
    clock_ticks_ = 0;
    ++clock_divider_;
    if (recording_)
      RecordStep(ioframe);
  }

  if (recording_) {
    BypassInputs(ioframe);
  } else if (ioframe->digital_inputs.triggered<OC::DIGITAL_INPUT_1>()) {
    AdvancePlayback();
  }

  if (smooth_ && !recording_) {
    const int next_step = NextStep(play_step_);
    const uint32_t period = clock_period_ ? clock_period_ : 1;
    const uint32_t phase = min(clock_ticks_, period);
    for (int track = 0; track < kTrackCount; ++track) {
      const int source = mode_ == REC_1_2 ? track % 2 : track;
      const int32_t current = RecordedValue(source, play_step_);
      const int32_t next = RecordedValue(source, next_step);
      output_[track] = current + static_cast<int32_t>(
        (static_cast<int64_t>(next - current) * phase) / period);
    }
  }

  for (int track = 0; track < kTrackCount; ++track)
    ioframe->outputs.set_pitch_value(track, output_[track]);

  if (++scope_tick_ >= 64) {
    scope_tick_ = 0;
    for (int track = 0; track < kTrackCount; ++track)
      scope_[track][scope_pos_] = static_cast<int16_t>(output_[track]);
    scope_pos_ = (scope_pos_ + 1) % kScopeWidth;
  }
}

void AppCVRecorder::GetIOConfig(OC::IOConfig &ioconfig) const {
  ioconfig.digital_inputs[0].set("Clock");
  ioconfig.digital_inputs[1].set("Clock /2");
  ioconfig.digital_inputs[2].set("Clock /4");
  ioconfig.digital_inputs[3].set("Start record");
  for (int channel = 0; channel < kTrackCount; ++channel) {
    ioconfig.cv[channel].set_printf("Record %d", channel + 1);
    ioconfig.outputs[channel].set_printf(OC::OUTPUT_MODE_PITCH, "Play %d", channel + 1);
  }
}

FLASHMEM void AppCVRecorder::HandleButtonEvent(const UI::Event &event) {
  if (event.type != UI::EVENT_BUTTON_PRESS)
    return;
  if (event.control == OC::CONTROL_BUTTON_A)
    smooth_ = !smooth_;
  else if (event.control == OC::CONTROL_BUTTON_B)
    StartRecording();
  else if (event.control == OC::CONTROL_BUTTON_L)
    mode_ = mode_ == REC_1_2 ? REC_1_2_3_4 : REC_1_2;
}

FLASHMEM void AppCVRecorder::HandleEncoderEvent(const UI::Event &event) {
  if (event.control == OC::CONTROL_ENCODER_L) {
    start_step_ = constrain(start_step_ + event.value, 0, end_step_ - 1);
    if (play_step_ < start_step_)
      play_step_ = start_step_;
  } else if (event.control == OC::CONTROL_ENCODER_R) {
    end_step_ = constrain(end_step_ + event.value, start_step_ + 1, kMaxStep - 1);
    if (play_step_ > end_step_)
      play_step_ = end_step_;
  }
}

void AppCVRecorder::DrawMenu() const {
  graphics.setPrintPos(2, 13);
  graphics.print(recording_ ? "REC" : "PLAY");
  graphics.setPrintPos(2, 24);
  graphics.print(mode_ == REC_1_2 ? "REC 1+2" : "REC 1+2+3+4");
  graphics.setPrintPos(2, 35);
  graphics.print("STEPS ");
  graphics.print(start_step_ + 1);
  graphics.print("-");
  graphics.print(end_step_ + 1);
  graphics.setPrintPos(2, 46);
  graphics.print(smooth_ ? "SMOOTH" : "STEP");
  graphics.setPrintPos(65, 46);
  graphics.print("NOW ");
  graphics.print(recording_ ? record_step_ + 1 : play_step_ + 1);
  const uint32_t period = clock_period_ ? clock_period_ : 1;
  const uint8_t frame = static_cast<uint8_t>((clock_ticks_ * 2UL) / period) & 1;
  graphics.drawBitmap8(108, 18, 16, recording_ ? CVREC_HORUS_REC : CVREC_HORUS_PLAY);
  if (!recording_ && frame)
    graphics.drawHLine(112, 21, 2);
}

void AppCVRecorder::DrawScreensaver() const {
  DrawScope();
}

void AppCVRecorder::DrawScope() const {
  graphics.setPrintPos(2, 8);
  graphics.print(recording_ ? "REC" : "PLAY");
  graphics.setPrintPos(32, 8);
  graphics.print(mode_ == REC_1_2 ? "1+2" : "1+2+3+4");
  const uint32_t period = clock_period_ ? clock_period_ : 1;
  const uint8_t frame = static_cast<uint8_t>((clock_ticks_ * 2UL) / period) & 1;
  graphics.drawBitmap8(108, 0, 16, recording_ ? CVREC_HORUS_REC : CVREC_HORUS_PLAY);
  if (!recording_ && frame)
    graphics.drawHLine(112, 3, 2);

  const int scope_height = 13;
  const int scope_top = 11;
  const int max_value = HEMISPHERE_MAX_CV;
  for (int track = 0; track < kTrackCount; ++track) {
    const int top = scope_top + track * scope_height;
    const int center = top + scope_height / 2;
    graphics.setPrintPos(1, top + 9);
    graphics.print(track + 1);
    if (track > 0)
      graphics.drawHLine(10, center, 118);

    int previous_y = center;
    for (int x = 0; x < kScopeWidth; ++x) {
      const int index = (scope_pos_ + x) % kScopeWidth;
      int value = constrain(static_cast<int>(scope_[track][index]), -max_value, max_value);
      int y = center - (value * (scope_height / 2 - 1)) / max_value;
      const int px = 12 + (x * 115) / (kScopeWidth - 1);
      graphics.drawLine(px - (x ? 2 : 0), previous_y, px, y);
      previous_y = y;
    }
  }
}

void AppCVRecorder::Loop() {
}

void AppCVRecorder::DrawDebugInfo() const {
}
