#!/usr/bin/env python3
import argparse
import subprocess
import sys
import os

# Add scripts to sys.path to import boards
sys.path.append(os.path.join(os.path.dirname(__file__), "scripts"))
from boards import SUPPORTED_BOARDS

BOARDS = list(SUPPORTED_BOARDS.keys())

STEPS = ["webapp", "splash", "firmware"]


def build_webapp():
    """Build the webapp (npm install + npm run build)."""
    print("\n=== Building webapp ===")
    try:
        subprocess.run("npm install", shell=True, check=True, cwd="webapp")
        subprocess.run("npm run build", shell=True, check=True, cwd="webapp")
    except subprocess.CalledProcessError as e:
        print(f"  ✗ Webapp build failed with exit code {e.returncode}")
        sys.exit(e.returncode)
    except FileNotFoundError:
        print("  ✗ 'npm' not found. Please ensure Node.js is installed and in your PATH.")
        sys.exit(1)


def generate_splash(board):
    """Generate splash screen EPDGZ for the target board."""
    print(f"\n=== Generating splash screen for {board} ===", flush=True)
    output_dir = os.path.join(os.path.dirname(__file__), "main", "splash_data")
    script = os.path.join(os.path.dirname(__file__), "scripts", "generate_splash.py")
    process_cli_dir = os.path.join(os.path.dirname(__file__), "process-cli")

    # Ensure process-cli dependencies are installed
    node_modules = os.path.join(process_cli_dir, "node_modules")
    if not os.path.isdir(node_modules):
        print("  Installing process-cli dependencies...")
        try:
            subprocess.run("npm ci", shell=True, check=True, cwd=process_cli_dir)
        except subprocess.CalledProcessError as e:
            print(f"  ✗ npm ci failed in process-cli with exit code {e.returncode}")
            sys.exit(e.returncode)

    try:
        subprocess.run(
            [sys.executable, script, "--board", board, "--output-dir", output_dir],
            check=True,
        )
    except subprocess.CalledProcessError as e:
        print(f"  ✗ Splash generation failed with exit code {e.returncode}")
        sys.exit(e.returncode)


def _idf_py():
    """Return command list to invoke idf.py.

    On Windows, idf.py is a Python script that must be run via the Python
    interpreter (shebang lines don't work natively). Prefer IDF_PATH env var
    to avoid picking up the binary idf.py.EXE wrapper via shutil.which.
    """
    import shutil
    idf_path = os.environ.get("IDF_PATH", "")
    if idf_path:
        idf_script = os.path.join(idf_path, "tools", "idf.py")
        if os.path.isfile(idf_script):
            if sys.platform == "win32":
                return [sys.executable, idf_script]
            return [idf_script]
    # Fallback: search PATH, but skip .EXE wrappers which can't be parsed as Python
    found = shutil.which("idf.py")
    if found and not found.lower().endswith(".exe"):
        if sys.platform == "win32":
            return [sys.executable, found]
        return [found]
    raise RuntimeError(
        "idf.py not found. Set IDF_PATH or activate ESP-IDF via export.ps1 / export.sh"
    )


def _read_idf_target(board):
    """Read CONFIG_IDF_TARGET from the board's sdkconfig.defaults, if present."""
    defaults_path = os.path.join(
        os.path.dirname(__file__), "boards", f"sdkconfig.defaults.{board}"
    )
    try:
        with open(defaults_path) as f:
            for line in f:
                line = line.strip()
                if line.startswith("CONFIG_IDF_TARGET="):
                    return line.split("=", 1)[1].strip().strip('"')
    except OSError:
        pass
    return None


def build_firmware(board, extra_args):
    """Build firmware with idf.py."""
    print(f"\n=== Building firmware for {board} ===")
    sdkconfig_defaults = f"sdkconfig.defaults;boards/sdkconfig.defaults.{board}"

    idf_target = _read_idf_target(board)
    idf_base = _idf_py() + [
        f"-DSDKCONFIG_DEFAULTS={sdkconfig_defaults}",
        "-DFIRMWARE_VERSION=2.10.11",
    ]
    if idf_target:
        idf_base.append(f"-DIDF_TARGET={idf_target}")

    cmake_defines = [a for a in extra_args if a.startswith("-D")]
    post_build_args = [a for a in extra_args if not a.startswith("-D")]

    build_cmd = idf_base + cmake_defines + ["build"]
    print(f"Running: {' '.join(build_cmd)}")

    try:
        subprocess.run(build_cmd, check=True)
    except subprocess.CalledProcessError as e:
        print(f"Build failed with exit code {e.returncode}")
        sys.exit(e.returncode)
    except (FileNotFoundError, RuntimeError) as e:
        print(f"Error: {e}")
        sys.exit(1)

    # Run post-build commands (flash, monitor, etc.)
    if post_build_args:
        post_cmd = idf_base + post_build_args
        print(f"Running: {' '.join(post_cmd)}")
        try:
            subprocess.run(post_cmd, check=True)
        except subprocess.CalledProcessError as e:
            print(f"Post-build command failed with exit code {e.returncode}")
            sys.exit(e.returncode)


def main():
    parser = argparse.ArgumentParser(description="Build firmware for different boards")
    parser.add_argument(
        "--board",
        choices=BOARDS,
        default="waveshare_photopainter_73",
        help="Board type to build",
    )
    parser.add_argument(
        "--fullclean",
        action="store_true",
        help="Remove sdkconfig and run idf.py fullclean before building",
    )
    parser.add_argument(
        "--step",
        choices=STEPS,
        action="append",
        help="Run only specific step(s). Can be specified multiple times. "
             "If omitted, all steps run.",
    )
    # Allow passing extra arguments to idf.py
    args, extra_args = parser.parse_known_args()

    steps = args.step if args.step else STEPS

    if args.fullclean:
        print("Performing full clean...")
        import shutil
        for f in ["sdkconfig", "partitions.csv"]:
            if os.path.exists(f):
                os.remove(f)
                print(f"  ✓ Removed {f}")
        if os.path.isdir("build"):
            shutil.rmtree("build")
            print("  ✓ Removed build/")

    if "webapp" in steps:
        build_webapp()

    if "splash" in steps:
        generate_splash(args.board)

    if "firmware" in steps:
        build_firmware(args.board, extra_args)


if __name__ == "__main__":
    main()
