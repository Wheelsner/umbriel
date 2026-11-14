#pragma once
#include "scene/surface_blur.h"

#include <list>
#include <memory>
#include <vector>
#include <wayland-server-core.h>

struct wlr_xdg_popup;
struct wlr_xdg_popup_configure;
struct wlr_scene_tree;
struct wlr_subsurface;
struct wlr_surface;

namespace umbriel {

  class Popup {
  public:
    explicit Popup(wlr_xdg_popup* popup, wlr_scene_tree* parentTree = nullptr, wlr_scene_tree* captureTree = nullptr);
    ~Popup();

    Popup(const Popup&) = delete;
    Popup& operator=(const Popup&) = delete;

  private:
    struct PopupSurfaceWatch {
      Popup* popup = nullptr;
      wlr_surface* surface = nullptr;
      wlr_subsurface* subsurface = nullptr;
      wl_listener commit{};
      wl_listener newSubsurface{};
      wl_listener subsurfaceDestroy{};
      wl_listener destroy{};
    };

    static void onCommit(wl_listener* listener, void* data);
    static void onReposition(wl_listener* listener, void* data);
    static void onConfigure(wl_listener* listener, void* data);
    static void onMap(wl_listener* listener, void* data);
    static void onUnmap(wl_listener* listener, void* data);
    static void onDestroy(wl_listener* listener, void* data);
    static void onSurfaceCommit(wl_listener* listener, void* data);
    static void onSurfaceNewSubsurface(wl_listener* listener, void* data);
    static void onSubsurfaceDestroy(wl_listener* listener, void* data);
    static void onSurfaceDestroy(wl_listener* listener, void* data);

    void updateBlur();
    void watchSurfaceTree(wlr_surface* root, wlr_subsurface* attachment = nullptr);
    void watchSurface(wlr_surface* surface, wlr_subsurface* attachment);
    void clearSurfaceWatches();
    void handleCommit();
    void handleReposition();
    void handleConfigure();
    void unconstrain();
    void handleMap();
    void handleDestroy();

    wlr_xdg_popup* m_popup = nullptr;
    SurfaceBlur m_blur;
    std::list<wlr_xdg_popup_configure> m_repositions;
    std::vector<std::unique_ptr<PopupSurfaceWatch>> m_surfaceWatches;

    wl_listener m_commit{};
    wl_listener m_reposition{};
    wl_listener m_configure{};
    wl_listener m_map{};
    wl_listener m_unmap{};
    wl_listener m_destroy{};
  };

} // namespace umbriel
