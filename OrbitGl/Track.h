// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ORBIT_GL_TRACK_H_
#define ORBIT_GL_TRACK_H_

#include <atomic>
#include <memory>
#include <vector>

#include "Batcher.h"
#include "BlockChain.h"
#include "CoreMath.h"
#include "OrbitBase/Profiling.h"
#include "PickingManager.h"
#include "TextBox.h"
#include "TextRenderer.h"
#include "TimeGraphLayout.h"
#include "TimerChain.h"
#include "TrackAccessibility.h"
#include "TriangleToggle.h"

class GlCanvas;
class TimeGraph;

class Track : public Pickable, public std::enable_shared_from_this<Track> {
 public:
  enum Type {
    kTimerTrack,
    kThreadTrack,
    kEventTrack,
    kFrameTrack,
    kGraphTrack,
    kGpuTrack,
    kSchedulerTrack,
    kAsyncTrack,
    kThreadStateTrack,
    kUnknown,
  };

  explicit Track(TimeGraph* time_graph);
  ~Track() override = default;
  virtual void Draw(GlCanvas* a_Canvas, PickingMode a_PickingMode, float z_offset = 0);
  virtual void UpdatePrimitives(uint64_t min_tick, uint64_t max_tick, PickingMode picking_mode,
                                float z_offset = 0);

  // Pickable
  void OnPick(int a_X, int a_Y) override;
  void OnRelease() override;
  void OnDrag(int a_X, int a_Y) override;
  [[nodiscard]] bool Draggable() override { return true; }

  [[nodiscard]] virtual Type GetType() const = 0;
  [[nodiscard]] virtual bool Movable() { return true; }

  [[nodiscard]] virtual float GetHeight() const { return 0.f; };
  [[nodiscard]] bool GetVisible() const { return visible_; }
  void SetVisible(bool value) { visible_ = value; }

  [[nodiscard]] uint32_t GetNumTimers() const { return num_timers_; }
  [[nodiscard]] virtual uint64_t GetMinTime() const { return min_time_; }
  [[nodiscard]] virtual uint64_t GetMaxTime() const { return max_time_; }
  void SetNumberOfPrioritizedTrailingCharacters(int num_characters) {
    num_prioritized_trailing_characters_ = num_characters;
  }
  [[nodiscard]] int GetNumberOfPrioritizedTrailingCharacters() const {
    return num_prioritized_trailing_characters_;
  }

  [[nodiscard]] virtual std::vector<std::shared_ptr<TimerChain>> GetTimers() const { return {}; }
  [[nodiscard]] virtual std::vector<std::shared_ptr<TimerChain>> GetAllChains() const { return {}; }
  [[nodiscard]] virtual std::vector<std::shared_ptr<TimerChain>> GetAllSerializableChains() const {
    return {};
  }

  [[nodiscard]] bool IsPinned() const { return pinned_; }
  void SetPinned(bool value);

  [[nodiscard]] bool IsMoving() const { return moving_; }
  [[nodiscard]] Vec2 GetMoveDelta() const {
    return moving_ ? mouse_pos_[1] - mouse_pos_[0] : Vec2(0, 0);
  }
  void SetName(const std::string& name) { name_ = name; }
  [[nodiscard]] const std::string& GetName() const { return name_; }
  void SetLabel(const std::string& label) { label_ = label; }
  [[nodiscard]] const std::string& GetLabel() const { return label_; }

  void SetTimeGraph(TimeGraph* timegraph) { time_graph_ = timegraph; }
  [[nodiscard]] TimeGraph* GetTimeGraph() { return time_graph_; }

  void SetPos(float a_X, float a_Y);
  void SetY(float y);
  [[nodiscard]] Vec2 GetPos() const { return pos_; }
  void SetSize(float a_SizeX, float a_SizeY);
  [[nodiscard]] Vec2 GetSize() const { return size_; }
  void SetColor(Color a_Color) { color_ = a_Color; }
  [[nodiscard]] Color GetBackgroundColor() const;

  [[nodiscard]] GlCanvas* GetCanvas() const { return canvas_; }

  void AddChild(std::shared_ptr<Track> track) { children_.emplace_back(track); }
  virtual void OnCollapseToggle(TriangleToggle::State state);
  [[nodiscard]] virtual bool IsCollapsable() const { return false; }
  [[nodiscard]] int32_t GetProcessId() const { return process_id_; }
  void SetProcessId(uint32_t pid) { process_id_ = pid; }
  [[nodiscard]] virtual bool IsEmpty() const = 0;

  [[nodiscard]] virtual bool IsTrackSelected() const { return false; }

  [[nodiscard]] bool IsCollapsed() const { return collapse_toggle_->IsCollapsed(); }

  // Accessibility
  [[nodiscard]] const orbit_gl::AccessibleTrack* AccessibilityInterface() const {
    return &accessibility_;
  }

 protected:
  void DrawTriangleFan(Batcher* batcher, const std::vector<Vec2>& points, const Vec2& pos,
                       const Color& color, float rotation, float z);

  GlCanvas* canvas_;
  TimeGraph* time_graph_;
  Vec2 pos_;
  Vec2 size_;
  Vec2 mouse_pos_[2];
  Vec2 picking_offset_;
  bool picked_;
  bool moving_;
  std::string name_;
  std::string label_;
  int num_prioritized_trailing_characters_;
  int32_t thread_id_;
  int32_t process_id_;
  Color color_;
  bool visible_ = true;
  bool pinned_ = false;
  std::atomic<uint32_t> num_timers_;
  std::atomic<uint64_t> min_time_;
  std::atomic<uint64_t> max_time_;
  bool picking_enabled_ = false;
  Type type_ = kUnknown;
  std::vector<std::shared_ptr<Track>> children_;
  std::shared_ptr<TriangleToggle> collapse_toggle_;

  orbit_gl::AccessibleTrack accessibility_;
};

#endif