#include "shell/drag_session.h"
#include "core/log.h"

namespace filo::shell {

using wheel::WheelKind;

bool DragSession::MatchWheel(uint32_t mods, WheelKind* out) const {
  // Exact match on the modifier set; a shortcut with an extra key is not a drag shortcut.
  if (conv_.vk == 0 && !conv_.IsEmpty() && conv_.mods == mods) { *out = WheelKind::Convert; return true; }
  if (tools_.vk == 0 && !tools_.IsEmpty() && tools_.mods == mods) { *out = WheelKind::Tools; return true; }
  if (actions_.vk == 0 && !actions_.IsEmpty() && actions_.mods == mods) { *out = WheelKind::Actions; return true; }
  return false;
}

void DragSession::Reset(bool animate) {
  svc_->KillTimer(kSettle); svc_->KillTimer(kGrace); svc_->KillTimer(kProbe);
  sawDragImage_ = false;
  if (state_ == State::WheelOpen || state_ == State::Closing) svc_->CloseWheel(animate);
  svc_->SuppressMenuActivation(false);
  state_ = State::Idle;
  prefetched_.clear();
}

void DragSession::OnLeftDown(POINT pt) {
  if (!enabled_) return;
  if (state_ == State::Closing) Reset(false);      // a new press while the previous wheel was fading
  if (state_ != State::Idle) return;
  state_ = State::Pressed;
  down_ = pt;
  downOnShell_ = svc_->IsShellViewAt(pt);
  prefetched_.clear();
}

void DragSession::OnDragMoved() {
  if (state_ != State::Pressed) return;
  state_ = State::Dragging;
  sawDragImage_ = svc_->DragImageWindowExists();
  svc_->ArmTimer(kProbe, 40);            // the drag image appears a few ms after the drag starts and only over drop targets
  svc_->SuppressMenuActivation(true);
  // Modifier already held when the drag began (e.g. Shift+drag): treat it as a trigger as well.
  if (mods_ != 0) { svc_->ArmTimer(kSettle, 25); settledMods_ = mods_; }
}

void DragSession::OnModsChanged(uint32_t mods) {
  mods_ = mods;
  if (!enabled_) return;
  if (state_ == State::Dragging || state_ == State::WheelOpen) {
    // Let chords settle (Ctrl+Alt arrive a few ms apart) before choosing a wheel.
    svc_->ArmTimer(kSettle, 25);
  }
}

void DragSession::TryOpen(uint32_t mods) {
  WheelKind k;
  if (!MatchWheel(mods, &k)) return;
  if (state_ == State::WheelOpen) {
    if (k != kind_) { kind_ = k; svc_->SwitchWheel(k); }
    return;
  }
  if (state_ != State::Dragging) return;
  // Only a genuine OLE drag may open the wheel (not rubber-band selection or text selection).
  if (!sawDragImage_ && !svc_->DragImageWindowExists()) return;
  POINT cur = svc_->CursorPos();
  if (downOnShell_) prefetched_ = svc_->PrefetchSelection(down_);
  kind_ = k;
  state_ = State::WheelOpen;
  svc_->OpenWheel(k, cur, prefetched_);
}

void DragSession::OnTimer(int id) {
  if (id == kProbe) {
    if (state_ != State::Dragging && state_ != State::WheelOpen) { svc_->KillTimer(kProbe); return; }
    if (!sawDragImage_ && svc_->DragImageWindowExists()) sawDragImage_ = true;
    return;                                // keeps firing (the timer is periodic) until the press ends
  }
  if (id == kSettle) { svc_->KillTimer(kSettle); TryOpen(mods_); }
  else if (id == kGrace) { svc_->KillTimer(kGrace); Reset(true); }
}

void DragSession::OnKeyDown(uint32_t vk) {
  if (vk == VK_ESCAPE && (state_ == State::WheelOpen || state_ == State::Dragging || state_ == State::Pressed)) {
    // The OLE drag cancels itself on Esc; we just take the wheel down immediately.
    Reset(true);
  }
}

void DragSession::OnLeftUp() {
  if (state_ == State::WheelOpen) {
    state_ = State::Closing;
    svc_->ArmTimer(kGrace, 350);         // wait for the drop target to receive Drop()/DragLeave()
  } else if (state_ != State::Idle) {
    Reset(false);
  }
}

void DragSession::OnDropHandled() {
  if (state_ == State::Closing || state_ == State::WheelOpen) Reset(true);
}

void DragSession::OnWheelClosedExternally() {
  svc_->KillTimer(kSettle); svc_->KillTimer(kGrace); svc_->KillTimer(kProbe);
  svc_->SuppressMenuActivation(false);
  state_ = State::Idle;
}

} // namespace filo::shell
