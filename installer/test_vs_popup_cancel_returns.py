import os
import subprocess
import sys
import time

import verify_installer_ui_pywinauto as ui


def find_execute_button():
    for window in ui.all_setup_windows():
        for control_id in ("Install", "InstallNoShield", "Repair", "Remove", "Change"):
            control = ui.find_visible(window, control_id)
            if control is not None:
                return control
        for name in ("Install", "Repair", "Remove", "Change"):
            control = ui.find_visible_button_by_name(window, name)
            if control is not None:
                return control
    return None


def main():
    if len(sys.argv) != 2:
        print("usage: test_vs_popup_cancel_returns.py <msi>")
        return 2

    msi_path = sys.argv[1]

    subprocess.run(["msiexec", "/x", msi_path, "/qn"], check=False)

    fake = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "Temp", "installer", "devenv.exe"))
    subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", f"Copy-Item 'C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe' '{fake}' -Force"], check=True)
    fake_proc = subprocess.Popen([fake, "-NoProfile", "-Command", "Start-Sleep -Seconds 300"])

    installer = subprocess.Popen([
        "msiexec",
        "/i",
        msi_path,
        "ALLUSERS=2",
        "MSIINSTALLPERUSER=1",
        "WixAppFolder=WixPerUserFolder",
    ])

    try:
        ui.wait_window(30)
        ui.click("Next", "Next")
        mode = ui.detect_flow_mode(20)
        if mode == "maintenance":
            ui.click("Repair", "Repair")
        else:
            ui.set_checked("LicenseAcceptedCheckBox")
            ui.click("Next", "Next")
            ui.click("Next", "Next")
            ui.click("Next", "Next")
            ui.click("ChangeFolder", "Change")
            ui.click("OK", "OK")
            ui.click("Next", "Next")

        button = find_execute_button()
        if button is None:
            raise RuntimeError("Execute button not found")
        ui.activate(button)

        if not ui.handle_vs_prompt("cancel", timeout=20):
            raise RuntimeError("Popup did not appear")

        time.sleep(2)
        if installer.poll() is not None:
            raise RuntimeError("Installer exited after popup cancel; expected to stay on setup dialog")

        if find_execute_button() is None:
            raise RuntimeError("Did not return to verify-ready action dialog after popup cancel")

        print("PASS: cancel closes popup and returns to setup dialog")
        return 0
    except Exception as exc:
        print(f"FAIL: {exc}")
        return 1
    finally:
        try:
            installer.terminate()
        except Exception:
            pass
        try:
            fake_proc.terminate()
        except Exception:
            pass
        subprocess.run(["msiexec", "/x", msi_path, "/qn"], check=False)


if __name__ == "__main__":
    raise SystemExit(main())
