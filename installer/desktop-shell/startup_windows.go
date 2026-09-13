package main

import (
	"context"
	"crypto/sha256"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"syscall"
	"time"
	"unsafe"

	"github.com/DerrickGold/ar-recomp/installer/desktop-shell/internal/host"
	"github.com/DerrickGold/ar-recomp/installer/desktop-shell/internal/winbundle"
	"golang.org/x/sys/windows"
)

var (
	splashUser32           = windows.NewLazySystemDLL("user32.dll")
	splashGDI32            = windows.NewLazySystemDLL("gdi32.dll")
	splashWindows          sync.Map // HWND -> *nativeStartup; no Go pointers retained by Win32.
	splashCallback         = syscall.NewCallback(splashWindowProc)
	splashActivateCallback = syscall.NewCallback(activateBuilderWindow)
)

const (
	wmSplashHandoff  = 0x8001
	wmSplashClose    = 0x8002
	wmSplashActivate = 0x8003
	wmClose          = 0x0010
	wmDestroy        = 0x0002
	wmTimer          = 0x0113
	wmCommand        = 0x0111
)

type splashClass struct {
	Size, Style                        uint32
	Proc                               uintptr
	ClassExtra, WindowExtra            int32
	Instance, Icon, Cursor, Background uintptr
	Menu, Name                         *uint16
	SmallIcon                          uintptr
}
type splashRect struct{ Left, Top, Right, Bottom int32 }
type splashMessage struct {
	Window         uintptr
	Message        uint32
	WParam, LParam uintptr
	Time           uint32
	X, Y           int32
	Private        uint32
}

type nativeStartup struct {
	mu       sync.Mutex
	progress winbundle.Progress
	hwnd     uintptr
	done     chan struct{}
	once     sync.Once
	cancel   context.CancelFunc
	// Remaining fields are owned by the locked native UI thread.
	status, bar, elapsed, button    uintptr
	started                         time.Time
	cancelled, handedOff, destroyed bool
}

// Keep UTF-16 buffers and native structs alive across this syscall wrapper.
//
//go:uintptrescapes
func splashCall(name string, args ...uintptr) uintptr {
	r, _, _ := splashUser32.NewProc(name).Call(args...)
	return r
}
func splashText(s string) *uint16 { p, _ := windows.UTF16PtrFromString(s); return p }

// Use an OS-owned lifetime token, not a PID/stamp file that can survive a crash.
// Holding the handle (without owning the mutex) keeps its name alive. The
// existing workspace/cache file locks remain the cross-version safety net.
func startupInstanceName(workspace, sid string) string {
	key := sid + "\x00" + strings.ToLower(filepath.Clean(workspace))
	return fmt.Sprintf("ActRaiserRecompBuilder-%x", sha256.Sum256([]byte(key)))
}

func beginStartup(override string, cancel context.CancelFunc) (startupWindow, error) {
	executable, err := os.Executable()
	if err != nil {
		return nil, err
	}
	workspace, err := host.DefaultWorkspace(executable, override)
	if err != nil {
		return nil, err
	}
	user, err := windows.GetCurrentProcessToken().GetTokenUser()
	if err != nil {
		return nil, err
	}
	name := startupInstanceName(workspace, user.User.Sid.String())
	guard, err := windows.CreateMutex(nil, false, splashText(`Local\`+name))
	if errors.Is(err, windows.ERROR_ALREADY_EXISTS) {
		defer windows.CloseHandle(guard)
		// A simultaneous launch can arrive between mutex and HWND creation.
		for deadline := time.Now().Add(time.Second); time.Now().Before(deadline); time.Sleep(25 * time.Millisecond) {
			if hwnd := splashCall("FindWindowW", uintptr(unsafe.Pointer(splashText(name))), 0); hwnd != 0 {
				var pid uint32
				splashCall("GetWindowThreadProcessId", hwnd, uintptr(unsafe.Pointer(&pid)))
				splashCall("AllowSetForegroundWindow", uintptr(pid))
				splashCall("PostMessageW", hwnd, wmSplashActivate, 0, 0)
				break
			}
		}
		return nil, errAlreadyRunning
	}
	if err != nil {
		return nil, fmt.Errorf("reserve Builder startup: %w", err)
	}
	s := &nativeStartup{done: make(chan struct{}), cancel: cancel, progress: winbundle.Progress{Stage: "Preparing to start"}}
	ready := make(chan error, 1)
	go func() {
		// Window creation, messages and destruction must share one OS thread.
		// Let Go retire this thread on return rather than reuse its Win32 queue.
		runtime.LockOSThread()
		defer close(s.done)
		defer windows.CloseHandle(guard)
		s.run(name, ready)
	}()
	if err := <-ready; err != nil {
		<-s.done
		return nil, err
	}
	return s, nil
}

func (s *nativeStartup) Update(p winbundle.Progress) {
	s.mu.Lock()
	s.progress = p
	s.mu.Unlock()
}

func (s *nativeStartup) post(message uintptr) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.hwnd != 0 {
		splashCall("PostMessageW", s.hwnd, message, 0, 0)
	}
}
func (s *nativeStartup) Handoff() { s.post(wmSplashHandoff) }
func (s *nativeStartup) Close() {
	s.once.Do(func() { s.post(wmSplashClose) })
	<-s.done
}

func (s *nativeStartup) run(name string, ready chan<- error) {
	// Keep scaling local to this thread. Wails/WebView2 owns its own DPI setup.
	dpi := splashUser32.NewProc("SetThreadDpiAwarenessContext")
	if dpi.Find() == nil {
		old, _, _ := dpi.Call(^uintptr(0)) // DPI_AWARENESS_CONTEXT_UNAWARE
		if old != 0 {
			defer dpi.Call(old)
		}
	}
	instance, _, _ := windows.NewLazySystemDLL("kernel32.dll").NewProc("GetModuleHandleW").Call(0)
	className := splashText(name)
	wc := splashClass{Proc: splashCallback, Instance: instance, Name: className, Background: 16,
		Cursor: splashCall("LoadCursorW", 0, 32512), Icon: splashCall("LoadIconW", instance, 1)}
	if wc.Icon == 0 {
		wc.Icon = splashCall("LoadIconW", 0, 32512)
	}
	wc.Size = uint32(unsafe.Sizeof(wc))
	if splashCall("RegisterClassExW", uintptr(unsafe.Pointer(&wc))) == 0 {
		ready <- errors.New("could not register the native Builder startup window")
		return
	}
	defer splashCall("UnregisterClassW", uintptr(unsafe.Pointer(className)), instance)
	const style = 0x00c80000   // Caption + system menu; no resizing/maximizing.
	const exStyle = 0x00040000 // Taskbar entry while the main window is unavailable.
	r := splashRect{Right: 510, Bottom: 276}
	splashCall("AdjustWindowRectEx", uintptr(unsafe.Pointer(&r)), style, 0, exStyle)
	work := splashRect{Right: 1024, Bottom: 768}
	splashCall("SystemParametersInfoW", 0x30, 0, uintptr(unsafe.Pointer(&work)), 0)
	width, height := r.Right-r.Left, r.Bottom-r.Top
	hwnd, _, createErr := splashUser32.NewProc("CreateWindowExW").Call(exStyle, uintptr(unsafe.Pointer(className)),
		uintptr(unsafe.Pointer(splashText("ActRaiser Recomp Builder"))), style,
		uintptr(work.Left+(work.Right-work.Left-width)/2), uintptr(work.Top+(work.Bottom-work.Top-height)/2),
		uintptr(width), uintptr(height), 0, 0, instance, 0)
	if hwnd == 0 {
		ready <- fmt.Errorf("create Builder startup window: %w", createErr)
		return
	}
	s.mu.Lock()
	s.hwnd = hwnd
	s.mu.Unlock()
	splashWindows.Store(hwnd, s)
	defer func() {
		s.mu.Lock()
		s.hwnd = 0
		s.mu.Unlock()
		if !s.destroyed {
			splashCall("DestroyWindow", hwnd)
		}
		splashWindows.Delete(hwnd)
	}()
	font, _, _ := splashGDI32.NewProc("GetStockObject").Call(17) // DEFAULT_GUI_FONT, not owned.
	add := func(class, text string, style uintptr, x, y, w, h int, id uintptr) uintptr {
		control := splashCall("CreateWindowExW", 0, uintptr(unsafe.Pointer(splashText(class))), uintptr(unsafe.Pointer(splashText(text))),
			0x50000000|style, uintptr(x), uintptr(y), uintptr(w), uintptr(h), hwnd, id, instance, 0)
		if control != 0 {
			splashCall("SendMessageW", control, 0x0030, font, 1)
		} // WM_SETFONT
		return control
	}
	title := add("STATIC", "Starting ActRaiser Recomp Builder", 0x80, 24, 22, 462, 26, 0)
	s.status = add("STATIC", "Preparing to start…", 0x80, 24, 64, 462, 38, 0)
	controls := struct{ Size, Classes uint32 }{8, 0x20} // ICC_PROGRESS_CLASS
	windows.NewLazySystemDLL("comctl32.dll").NewProc("InitCommonControlsEx").Call(uintptr(unsafe.Pointer(&controls)))
	s.bar = add("msctls_progress32", "", 0, 24, 110, 462, 16, 0)
	help := add("STATIC", "The first launch prepares the bundled tools and browser.\r\nThis can take a few minutes. There is no need to open another copy.", 0x80, 24, 146, 462, 42, 0)
	s.elapsed = add("STATIC", "Starting…", 0x80, 24, 227, 350, 24, 0)
	s.button = add("BUTTON", "Cancel", 0x10000, 398, 218, 88, 30, 1) // WS_TABSTOP
	if title == 0 || s.status == 0 || s.bar == 0 || help == 0 || s.elapsed == 0 || s.button == 0 {
		ready <- errors.New("could not create the native Builder startup controls")
		return
	}
	s.started = time.Now()
	s.render()
	if splashCall("SetTimer", hwnd, 1, 200, 0) == 0 {
		ready <- errors.New("could not start the Builder startup progress timer")
		return
	}
	defer splashCall("KillTimer", hwnd, 1)
	splashCall("ShowWindow", hwnd, 5)
	splashCall("UpdateWindow", hwnd) // Paint before any archive IO starts.
	splashCall("SetForegroundWindow", hwnd)
	ready <- nil
	var msg splashMessage
	for {
		result := int32(splashCall("GetMessageW", uintptr(unsafe.Pointer(&msg)), 0, 0, 0))
		if result <= 0 {
			if result < 0 {
				s.cancel()
			}
			return
		}
		if splashCall("IsDialogMessageW", hwnd, uintptr(unsafe.Pointer(&msg))) == 0 {
			splashCall("TranslateMessage", uintptr(unsafe.Pointer(&msg)))
			splashCall("DispatchMessageW", uintptr(unsafe.Pointer(&msg)))
		}
	}
}

func (s *nativeStartup) render() {
	s.mu.Lock()
	p := s.progress
	s.mu.Unlock()
	status := p.Stage + "…"
	if s.cancelled {
		status = "Cancelling startup and cleaning up temporary files…"
		p.Total = 0
	}
	if p.Total > 0 {
		percent := max(int64(0), min(int64(100), p.Completed*100/p.Total))
		status = fmt.Sprintf("%s — %d%%", p.Stage, percent)
		splashCall("SendMessageW", s.bar, 0x0402, uintptr(percent), 0) // PBM_SETPOS
		splashCall("ShowWindow", s.bar, 5)
	} else {
		splashCall("ShowWindow", s.bar, 0)
	}
	splashCall("SetWindowTextW", s.status, uintptr(unsafe.Pointer(splashText(status))))
	seconds := int(time.Since(s.started).Seconds())
	activity := []string{"·", "··", "···"}[int(time.Since(s.started)/(400*time.Millisecond))%3]
	text := fmt.Sprintf("%d:%02d elapsed  %s", seconds/60, seconds%60, activity)
	splashCall("SetWindowTextW", s.elapsed, uintptr(unsafe.Pointer(splashText(text))))
}

func splashWindowProc(hwnd uintptr, message uint32, wParam, lParam uintptr) uintptr {
	value, ok := splashWindows.Load(hwnd)
	if !ok {
		return splashCall("DefWindowProcW", hwnd, uintptr(message), wParam, lParam)
	}
	s := value.(*nativeStartup)
	switch message {
	case wmTimer:
		if !s.handedOff {
			s.render()
		}
		return 0
	case wmCommand:
		if wParam&0xffff != 1 && wParam&0xffff != 2 {
			break
		} // Cancel / Escape
		fallthrough
	case wmClose:
		if !s.handedOff && !s.cancelled {
			s.cancelled = true
			splashCall("EnableWindow", s.button, 0)
			s.cancel()
			s.render()
		}
		return 0 // Keep visible until cancellation has removed the staging tree.
	case wmSplashHandoff:
		s.handedOff = true
		splashCall("KillTimer", hwnd, 1)
		splashCall("ShowWindow", hwnd, 0)
		return 0 // Keep the hidden HWND as the second-launch activation target.
	case wmSplashActivate:
		if s.handedOff {
			splashCall("EnumWindows", splashActivateCallback, hwnd)
		} else {
			splashCall("SetForegroundWindow", hwnd)
		}
		return 0
	case wmSplashClose:
		splashCall("DestroyWindow", hwnd)
		return 0
	case wmDestroy:
		s.destroyed = true
		s.mu.Lock()
		s.hwnd = 0
		s.mu.Unlock()
		splashCall("PostQuitMessage", 0)
		return 0
	}
	return splashCall("DefWindowProcW", hwnd, uintptr(message), wParam, lParam)
}

func activateBuilderWindow(hwnd, splash uintptr) uintptr {
	var pid uint32
	splashCall("GetWindowThreadProcessId", hwnd, uintptr(unsafe.Pointer(&pid)))
	if hwnd != splash && pid == windows.GetCurrentProcessId() && splashCall("IsWindowVisible", hwnd) != 0 {
		if splashCall("IsIconic", hwnd) != 0 {
			splashCall("ShowWindow", hwnd, 9)
		}
		splashCall("SetForegroundWindow", hwnd)
		return 0
	}
	return 1
}
