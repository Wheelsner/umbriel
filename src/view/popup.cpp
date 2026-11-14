#include "view/popup.h"

#include "layer/layer_surface.h"
#include "output/output.h"
#include "scene/node.h"
#include "view/view.h"
#include "wlr.h"

#include <algorithm>
#include <cassert>
#include <ranges>

namespace umbriel {

  Popup::Popup(wlr_xdg_popup* popup, wlr_scene_tree* parentTree, wlr_scene_tree* captureTree) : m_popup(popup) {
    if (parentTree == nullptr) {
      wlr_xdg_surface* parent = wlr_xdg_surface_try_from_wlr_surface(m_popup->parent);
      assert(parent != nullptr);
      parentTree = static_cast<wlr_scene_tree*>(parent->data);
    }
    m_popup->base->data = wlr_scene_xdg_surface_create(parentTree, m_popup->base);
    if (captureTree != nullptr) {
      wlr_scene_xdg_surface_create(captureTree, m_popup->base);
    }

    watchSurfaceTree(m_popup->base->surface);
    m_commit.notify = onCommit;
    wl_signal_add(&m_popup->base->surface->events.commit, &m_commit);
    m_reposition.notify = onReposition;
    wl_signal_add(&m_popup->events.reposition, &m_reposition);
    m_configure.notify = onConfigure;
    wl_signal_add(&m_popup->base->events.configure, &m_configure);
    m_map.notify = onMap;
    wl_signal_add(&m_popup->base->surface->events.map, &m_map);
    m_unmap.notify = onUnmap;
    wl_signal_add(&m_popup->base->surface->events.unmap, &m_unmap);
    m_destroy.notify = onDestroy;
    wl_signal_add(&m_popup->events.destroy, &m_destroy);
  }

  Popup::~Popup() {
    clearSurfaceWatches();
    if (m_commit.link.next != nullptr) {
      wl_list_remove(&m_commit.link);
      wl_list_remove(&m_reposition.link);
      wl_list_remove(&m_configure.link);
      wl_list_remove(&m_map.link);
      wl_list_remove(&m_unmap.link);
      wl_list_remove(&m_destroy.link);
    }
  }

  void Popup::onCommit(wl_listener* listener, void* /*data*/) {
    Popup* self = wl_container_of(listener, self, m_commit); // NOLINT(modernize-use-auto)
    self->handleCommit();
  }

  void Popup::onReposition(wl_listener* listener, void* /*data*/) {
    Popup* self = wl_container_of(listener, self, m_reposition); // NOLINT(modernize-use-auto)
    self->handleReposition();
  }

  void Popup::onConfigure(wl_listener* listener, void* /*data*/) {
    Popup* self = wl_container_of(listener, self, m_configure); // NOLINT(modernize-use-auto)
    self->handleConfigure();
  }

  void Popup::onMap(wl_listener* listener, void* /*data*/) {
    Popup* self = wl_container_of(listener, self, m_map); // NOLINT(modernize-use-auto)
    self->handleMap();
  }

  void Popup::onUnmap(wl_listener* listener, void* /*data*/) {
    Popup* self = wl_container_of(listener, self, m_unmap); // NOLINT(modernize-use-auto)
    self->m_blur.hide();
    self->m_repositions.clear();
  }

  void Popup::onDestroy(wl_listener* listener, void* /*data*/) {
    Popup* self = wl_container_of(listener, self, m_destroy); // NOLINT(modernize-use-auto)
    self->handleDestroy();
  }

  void Popup::onSurfaceCommit(wl_listener* listener, void* /*data*/) {
    PopupSurfaceWatch* watch = wl_container_of(listener, watch, commit); // NOLINT(modernize-use-auto)
    if (watch->subsurface != nullptr) {
      watch->popup->updateBlur();
    }
  }

  void Popup::onSurfaceNewSubsurface(wl_listener* listener, void* data) {
    PopupSurfaceWatch* watch = wl_container_of(listener, watch, newSubsurface); // NOLINT(modernize-use-auto)
    auto* subsurface = static_cast<wlr_subsurface*>(data);
    watch->popup->watchSurfaceTree(subsurface->surface, subsurface);
    watch->popup->updateBlur();
  }

  void Popup::onSubsurfaceDestroy(wl_listener* listener, void* /*data*/) {
    PopupSurfaceWatch* watch = wl_container_of(listener, watch, subsurfaceDestroy); // NOLINT(modernize-use-auto)
    watch->subsurface = nullptr;
    wl_list_remove(&watch->subsurfaceDestroy.link);
    watch->popup->updateBlur();
  }

  void Popup::onSurfaceDestroy(wl_listener* listener, void* /*data*/) {
    PopupSurfaceWatch* watch = wl_container_of(listener, watch, destroy); // NOLINT(modernize-use-auto)
    Popup* popup = watch->popup;
    wl_list_remove(&watch->commit.link);
    wl_list_remove(&watch->newSubsurface.link);
    if (watch->subsurface != nullptr) {
      wl_list_remove(&watch->subsurfaceDestroy.link);
    }
    wl_list_remove(&watch->destroy.link);
    std::erase_if(popup->m_surfaceWatches, [watch](const auto& candidate) { return candidate.get() == watch; });
  }

  void Popup::watchSurfaceTree(wlr_surface* root, wlr_subsurface* attachment) {
    if (root == nullptr) {
      return;
    }
    watchSurface(root, attachment);

    wlr_subsurface* child;
    wl_list_for_each(child, &root->current.subsurfaces_below, current.link) {
      watchSurfaceTree(child->surface, child);
    }
    wl_list_for_each(child, &root->current.subsurfaces_above, current.link) {
      watchSurfaceTree(child->surface, child);
    }
  }

  void Popup::watchSurface(wlr_surface* surface, wlr_subsurface* attachment) {
    if (surface == nullptr) {
      return;
    }
    const auto existing =
        std::ranges::find_if(m_surfaceWatches, [surface](const auto& watch) { return watch->surface == surface; });
    if (existing != m_surfaceWatches.end()) {
      PopupSurfaceWatch& watch = **existing;
      if (watch.subsurface == nullptr && attachment != nullptr) {
        watch.subsurface = attachment;
        watch.subsurfaceDestroy.notify = onSubsurfaceDestroy;
        wl_signal_add(&attachment->events.destroy, &watch.subsurfaceDestroy);
      }
      return;
    }

    auto watch = std::make_unique<PopupSurfaceWatch>();
    watch->popup = this;
    watch->surface = surface;
    watch->subsurface = attachment;
    watch->commit.notify = onSurfaceCommit;
    wl_signal_add(&surface->events.commit, &watch->commit);
    watch->newSubsurface.notify = onSurfaceNewSubsurface;
    wl_signal_add(&surface->events.new_subsurface, &watch->newSubsurface);
    if (watch->subsurface != nullptr) {
      watch->subsurfaceDestroy.notify = onSubsurfaceDestroy;
      wl_signal_add(&watch->subsurface->events.destroy, &watch->subsurfaceDestroy);
    }
    watch->destroy.notify = onSurfaceDestroy;
    wl_signal_add(&surface->events.destroy, &watch->destroy);
    m_surfaceWatches.push_back(std::move(watch));
  }

  void Popup::clearSurfaceWatches() {
    for (const auto& watch : m_surfaceWatches) {
      wl_list_remove(&watch->commit.link);
      wl_list_remove(&watch->newSubsurface.link);
      if (watch->subsurface != nullptr) {
        wl_list_remove(&watch->subsurfaceDestroy.link);
      }
      wl_list_remove(&watch->destroy.link);
    }
    m_surfaceWatches.clear();
  }

  void Popup::updateBlur() {
    if (m_popup == nullptr || m_popup->base == nullptr || m_popup->base->data == nullptr) {
      return;
    }
    const wlr_box& geometry = m_popup->base->geometry;
    SurfaceBlurOptions blurOptions;
    if (LayerSurface* layer = LayerSurface::fromSurface(m_popup->parent)) {
      blurOptions = layer->popupBlurOptions();
    } else if (View* view = View::fromSurface(m_popup->parent)) {
      blurOptions = view->popupBlurOptions();
    }
    m_blur.update(
        static_cast<wlr_scene_tree*>(m_popup->base->data), m_popup->base->surface,
        wlr_box{0, 0, geometry.width, geometry.height}, geometry, corner_radii_none(), nullptr, blurOptions
    );
  }

  void Popup::handleCommit() {
    updateBlur();
    if (!m_popup->base->initial_commit) {
      return;
    }

    // New popups miss the parent's map/workspace-time scale notify; tell them the parent's output scale before the
    // first configure so scale-aware clients size and map input correctly from frame one.
    Output* output = nullptr;
    if (LayerSurface* layer = LayerSurface::fromSurface(m_popup->parent)) {
      output = layer->output();
    } else if (View* view = View::fromSurface(m_popup->parent)) {
      output = view->currentOutput();
    }
    if (output != nullptr) {
      wlr_xdg_surface_for_each_surface(m_popup->base, &Output::notifySurfaceScaleIter, output);
    }

    unconstrain();
  }

  void Popup::handleReposition() {
    unconstrain();
    // Clients can wait for a particular token before drawing. Preserve each request's placement and token until
    // wlroots sends its configure, then schedule the next request through the same configure path.
    m_repositions.push_back(m_popup->scheduled);
    m_popup->scheduled = m_repositions.front();
  }

  void Popup::handleConfigure() {
    if (m_repositions.empty()) {
      return;
    }
    m_repositions.pop_front();
    if (!m_repositions.empty()) {
      m_popup->scheduled = m_repositions.front();
      unconstrain();
    }
  }

  void Popup::unconstrain() {
    if (LayerSurface* layer = LayerSurface::fromSurface(m_popup->parent)) {
      layer->unconstrainPopup(m_popup);
    } else if (View* view = View::fromSurface(m_popup->parent)) {
      view->unconstrainPopup(m_popup);
    }
    // Unconstraining schedules the configure, but it is skipped while the parent is not shown or has no output. A
    // client must not draw before its first configure, and some block until it arrives. A configure already
    // scheduled is reused, not sent twice.
    wlr_xdg_surface_schedule_configure(m_popup->base);
  }

  void Popup::handleMap() {
    // Only a grabbing popup owns the keyboard. A toplevel's popup reaches it through its already focused window.
    if (m_popup->seat == nullptr) {
      return;
    }
    if (LayerSurface* layer = LayerSurface::fromSurface(m_popup->parent)) {
      layer->focusForPopupGrab();
    }
  }

  void Popup::handleDestroy() {
    clearSurfaceWatches();
    wl_list_remove(&m_commit.link);
    wl_list_remove(&m_reposition.link);
    wl_list_remove(&m_configure.link);
    wl_list_remove(&m_map.link);
    wl_list_remove(&m_unmap.link);
    wl_list_remove(&m_destroy.link);
    m_commit.link.next = nullptr;
    m_reposition.link.next = nullptr;
    m_configure.link.next = nullptr;
    m_map.link.next = nullptr;
    m_unmap.link.next = nullptr;
    m_destroy.link.next = nullptr;
    delete this;
  }

} // namespace umbriel
