import argparse
import subprocess
import sys
import time

from pywinauto import Desktop


def wait_setup_window(timeout=30):
    end = time.time() + timeout
    desktop = Desktop(backend="win32")
    while time.time() < end:
        for w in desktop.windows():
            try:
                if w.window_text() and w.window_text().endswith("Setup"):
                    return w
            except Exception:
                pass
        time.sleep(0.2)
    return None


def click_button_contains(window, text_part, timeout=20):
    end = time.time() + timeout
    needle = text_part.lower()
    while time.time() < end:
        for b in window.descendants(class_name="Button"):
            try:
                text = (b.window_text() or "").lower().replace("&", "")
                if b.is_visible() and needle in text:
                    b.click()
                    return True
            except Exception:
                pass
        time.sleep(0.2)
    return False


def visible_button_labels(window):
    labels = []
    for b in window.descendants(class_name="Button"):
        try:
            if b.is_visible():
                labels.append((b.window_text() or "").replace("&", ""))
        except Exception:
            pass
    return labels


def run(msi_path):
    proc = subprocess.Popen(["msiexec", "/i", msi_path])
    try:
        w = wait_setup_window(30)
        if w is None:
            raise RuntimeError("Setup window did not appear")

        # Reach maintenance action dialog regardless of entry screen.
        for _ in range(3):
            print("buttons:", visible_button_labels(w))
            if click_button_contains(w, "remove", 2):
                break
            if not click_button_contains(w, "next", 5):
                break
            w = wait_setup_window(10)
            if w is None:
                break

        if w is None:
            raise RuntimeError("Setup window disappeared before remove flow")

        if not click_button_contains(w, "remove", 5):
            print("buttons at maintenance:", visible_button_labels(w))
            raise RuntimeError("Remove button not found on maintenance dialog")

        w = wait_setup_window(10)
        if w is None:
            raise RuntimeError("Did not reach ready dialog")

        if not (click_button_contains(w, "remove", 5) or click_button_contains(w, "install", 5)):
            raise RuntimeError("Final remove action button not found on ready dialog")

        end = time.time() + 120
        while time.time() < end:
            if wait_setup_window(1) is None:
                return 0
            time.sleep(0.25)

        raise RuntimeError("Installer did not exit after remove")
    finally:
        try:
            proc.poll()
            if proc.returncode is None:
                proc.terminate()
        except Exception:
            pass


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--msi", required=True)
    args = parser.parse_args()
    try:
        code = run(args.msi)
        print("PASS: maintenance remove flow executed")
        return code
    except Exception as exc:
        print(f"FAIL: {exc}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
