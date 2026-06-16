# 🎮 EasyMap: Ultra-Low Latency Projection Mapping & Window Warping for Windows
### *The Ultimate Projector Integration Tool for High-Refresh Rate Gaming & Multi-Monitor Setups*

<img width="806" height="697" alt="image" src="https://github.com/user-attachments/assets/53a870d3-aef2-4d5d-972d-ed9f42644401" />


Welcome to **EasyMap**, a hardware-accelerated, real-time window capturing and projection mapping utility designed from the ground up for gamers. Whether you are throwing a 150-inch display onto an unequal wall for an immersive flight simulator, mapping a racing game to a curved custom screen, or setting up a couch-gaming projector alongside your primary desktop monitor, EasyMap ensures your frames flow flawlessly with **near-zero input lag** and **maximum refresh rate utilization**.

Unlike traditional streaming, recording, or window-mirroring software that introduces heavy CPU overhead, frame stuttering, and massive display latency, EasyMap leverages direct GPU-bound subsystems to capture, warp, crop, and present your games in real time.

---


##  Just download the .exe run configure and play! Or build the app! 




## ⚡ Why EasyMap is Built for Gamers

When gaming on a projector—especially in multi-monitor setups—you traditionally face three massive hurdles: **Input Lag**, **Refresh Rate Caps**, and **Window Focus Loss (The Alt-Tab Problem)**. EasyMap obliterates these bottlenecks through cutting-edge Win32, DX11, and C++/WinRT optimizations.

### 1. Input Lag Annihilation (Waitable Swap Chains)
Standard window mirroring causes frames to stack up in driver queues, adding 2 to 4 frames of systemic input lag (~33ms–66ms at 60Hz), making fast-paced games unplayable. EasyMap uses a **DXGI Frame Latency Waitable Object** (`DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT`) combined with `SetMaximumFrameLatency(1)`. This forces the graphics engine to sync directly with your projector’s physical hardware VSync interval with near-zero jitter, bypassing the standard presentation queue entirely.

### 2. High-Refresh Rate Protection (120Hz, 144Hz, 240Hz+)
EasyMap is fully **Per-Monitor V2 DPI Aware**. It queries your projector’s exact physical desktop topology and supports high refresh rates seamlessly. By utilizing low-overhead hardware vertex structures and processing the mapping algorithm natively on the GPU, your presentation layer will scale seamlessly to match the exact maximum refresh rate of your high-end gaming projector.

### 3. The "Alt-Tab Proof" Invisible Capture Trick
In multi-monitor setups, clicking on a secondary screen (like EasyMap's Control Panel or a chat app) causes standard games to minimize or drop their rendering frame rate to 10–30 FPS. EasyMap solves this via a sophisticated Win32 window-hacking technique:
* When a window is minimized or loses focus, EasyMap intercepts it and applies `WS_EX_LAYERED | WS_EX_TRANSPARENT` styles.
* It sets the window opacity to **0 (completely transparent and click-through)** and forces it to `HWND_TOPMOST`.
* Because the window remains un-minimized and fully composited on the desktop by Windows Desktop Window Manager (DWM), **the game continues to render at full speed (e.g., 144+ FPS) in the background**, allowing Windows Graphics Capture (WGC) to stream its frames flawlessly while you use your other monitors!

### 4. Zero CPU Stall GPU-Side Capturing
EasyMap utilizes the modern **Windows Graphics Capture (WGC)** API. Instead of pulling pixels from the GPU back to system memory (CPU), which introduces severe bottlenecks, EasyMap creates an exclusive, owned Direct3D11 texture on the GPU. Frame copying happens completely hardware-side via `CopyResource`, freeing your CPU to focus entirely on your game's physics and logic threads.

---

## 🚀 Core Feature Breakdown

* **Real-Time Homography Warping:** Map your game window to any arbitrary four-corner quad directly on your screen to counteract projector tilt, keystoning, or surface geometry distortion.
* **Pixel-Perfect Shader Cropping:** Built-in HLSL-driven margins allow you to clip out annoying window title bars, thick window borders, or specific UI elements without performance loss.
* **On-the-Fly Fine Tuning:** Select individual quad corners and use your keyboard arrow keys (holding `Shift` for 10px precision steps) to lock in your calibration down to the exact pixel.
* **Interactive Alignment Grid:** Overlay a highly visible, custom-colored alignment grid with flexible column and row dimensions to match your physical projection surface boundaries perfectly.

---

## 🛠️ System Requirements & Prerequisites

* **Operating System:** Windows 10 (Build 2004 / Version 2004 or newer) or Windows 11.
* **Graphics Hardware:** NVIDIA GeForce, AMD Radeon, or Intel Arc GPU supporting DirectX 11.
* **Dependencies:** Direct3D 11 runtime environment.
* **Game Configuration:** Games must be run in **Borderless Windowed** or **Windowed** mode. (Exclusive Fullscreen prevents Windows Graphics Capture from accessing the frame buffer).

---

## 🗺️ Step-by-Step Setup Guide for Gamers

### Step 1: Configure Your Windows Displays
1. Ensure your gaming projector is connected, powered on, and recognized by Windows as an **Extended Display** (do not use "Duplicate these displays").
2. Right-click your desktop, go to **Display Settings**, click on your projector display, and scroll down to **Advanced Display**.
3. Set the **Choose a refresh rate** dropdown to the absolute maximum supported by your hardware (e.g., `120Hz` or `144Hz`).

### Step 2: Launch EasyMap & Open the Projector Window
1. Open the EasyMap executable. You will be greeted by the dark-themed Control Panel.
2. In section **1. Output Projector Display**, look at the **Select Display** dropdown. Choose the index corresponding to your projector (it will list the monitor's technical device name and current resolution).
3. Click **Open Projector Window**. A completely black, borderless window will open and span across your projector's entire display workspace.

### Step 3: Launch and Hook Your Game
1. Start your game. Go to its video options and change the display mode to **Windowed** or **Borderless Windowed / Borderless**.
2. Alt-Tab back to the EasyMap Control Panel. In section **2. Source Capture Window**, click **Refresh Windows**.
3. Expand the **Select Window** dropdown and pick your game from the list.
4. EasyMap will instantly begin pulling frames directly from the game's GPU buffer.

### Step 4: Calibrate and Warp the Image
1. In the Control Panel, check the box for **Show Corner Handles & Guides**. You will see cyan handles appear at the corners of your projector display.
2. If your game has structural borders or title bars showing up, go to section **5. Image Cropping Options** and click **Crop Standard Window Borders**, or adjust the Top/Bottom/Left/Right sliders manually until the game content fills your capture box cleanly.
3. Walk up to your projection surface or watch the output carefully. Grab the handles on your projector screen using your mouse and drag them to line up with the edges of your projector screen or physical wall frame.
4. **For Pixel-Perfect Alignment:** Go to the Control Panel under section **3. Projection Mapping**, choose a corner from the **Fine-tune Corner** dropdown, and tap your keyboard's **Arrow Keys** to nudge the corner pixel-by-pixel. Hold down **Shift** to jump by 10 pixels at a time.
5. Once aligned, turn off **Show Corner Handles & Guides** and click **Save Calibration Settings** to lock your setup into `config.txt`.

---

## 🔧 Multi-Monitor Pro-Tips for Maximum Performance

### Keeping Frames Flowing with the "Transparency Hack"
If you intend to play your game on the projector while interacting with an app (like Discord, a walkthrough, or a stream manager) on your main monitor:
1. Ensure **Auto-keep minimized windows active (transparent)** is checked in section 2.
2. If you click away from the game and notice it stops updating or drops frames, simply click the **Make window transparent (keep capturing)** button in EasyMap.
3. This shifts the game window into an active, high-priority, invisible state. Windows will keep rendering it at maximum frame rate, and your projector display will stay silky smooth while your mouse travels freely over your secondary monitors!

### Minimizing Latency and Stutter
* **Match Frame Rates:** If possible, set an in-game frame rate cap that matches your projector's refresh rate (e.g., cap at 120 FPS for a 120Hz projector). This prevents frame pacing mismatch and reduces micro-stuttering.
* **Run as Administrator:** If your game runs with elevated administrative privileges, EasyMap's Control Panel must also be run as an Administrator so the Windows Graphics Capture system can hook into its window handle.
* **Check the Performance Monitor:** Expand section **6. Performance Debug** in the Control Panel. Keep an eye on `Render FPS` and `Dropped (cap)`. If dropped frames are climbing while your game is active, ensure the game is not minimized to the taskbar normally.

---

## 📄 Understanding the Configuration File (`config.txt`)

All of your hard-earned alignment values are saved locally into a plaintext file named `config.txt` right next to the app executable. You can back this file up or manually edit properties if needed. 

Key parameters inside `config.txt`:
* `monitorIndex`: The Windows monitor index mapping to your projector target.
* `captureWindowTitle`: The exact text title of your game window for auto-hooking on launch.
* `cornerX_x / cornerX_y`: Pixel-coordinate mapping points for your four physical warping boundaries.
* `autoRestoreMinimized`: Set to `1` to automate the background transparent rendering trick.
* `cropTop / cropBottom / cropLeft / cropRight`: Fractional cropping data (ranges from `0.0` to `0.25`) removing outermost window margins.

---

## 🛠️ Troubleshooting & FAQ

#### Q: I selected my game but the projector screen is totally black or stuck on "Waiting for frames..."
**A:** This is almost always caused by one of two things:
1. Your game is running in **Exclusive Fullscreen** mode. Change your game's video settings to **Windowed** or **Borderless Windowed**.
2. The game has higher user-account permissions than EasyMap. Close EasyMap, right-click its executable, and select **Run as Administrator**.

#### Q: My game's frame rate tanks or feels sluggish when I click onto my second monitor.
**A:** Many modern game engines automatically throttle performance when they lose window focus to save power. Enable **Auto-keep minimized windows active (transparent)** in EasyMap, and use the **Make window transparent** function. This tricks Windows into thinking the game is active and topmost on your desktop screen, forcing full-speed GPU execution.

#### Q: The performance overlay shows high "Present time" spikes. Is this bad?
**A:** No! Because EasyMap implements low-latency waitable objects linked directly to your projector's hardware synchronization engine, a high presentation time simply indicates that the app is successfully waiting for the projector's physical hardware VSync refresh line. This is completely normal and means the synchronization system is working perfectly.

---
*Happy Gaming! Optimize your bounds, lock your calibration, and enjoy lag-free projection mapping.*
