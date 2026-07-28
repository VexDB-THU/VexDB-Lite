#!/usr/bin/env python3
"""Guard the release iOS bundle against image and package-size regressions."""

import pathlib
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
DEFAULT_APP = ROOT / "build-device" / "Release-iphoneos" / "VexDB Lite.app"
APP_LIMIT = 7 * 1024 * 1024
DEMO_IMAGE_LIMIT = 1024 * 1024


def total_file_bytes(path: pathlib.Path) -> int:
    return sum(item.stat().st_size for item in path.rglob("*") if item.is_file())


def main() -> None:
    app = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_APP
    if not app.is_dir():
        raise SystemExit("iOS App bundle not found: {}".format(app))

    expected_jpg = {"image-test-img{:02d}.jpg".format(index) for index in range(1, 21)}
    actual_jpg = {item.name for item in app.glob("image-test-img*.jpg")}
    assert actual_jpg == expected_jpg, "unexpected JPEG resources: {}".format(
        sorted(actual_jpg)
    )
    stale_demo = [item.name for pattern in (
        "image-demo-*.png", "image-demo-*.jpg", "image-demo-*.webp",
        "image-test-img*.webp",
    )
                  for item in app.glob(pattern)]
    assert not stale_demo, "stale demo image files are still bundled: {}".format(stale_demo)

    for name in expected_jpg:
        header = (app / name).read_bytes()[:3]
        assert header == b"\xff\xd8\xff", "invalid JPEG resource: {}".format(name)

    demo_images = [app / name for name in expected_jpg]
    image_bytes = sum(item.stat().st_size for item in demo_images)
    app_bytes = total_file_bytes(app)
    assert image_bytes <= DEMO_IMAGE_LIMIT, \
        "demo images exceed 1 MiB: {} bytes".format(image_bytes)
    assert app_bytes <= APP_LIMIT, \
        "release App exceeds 7 MiB: {} bytes".format(app_bytes)

    print("IOS BUNDLE PASS: app={:.2f} MiB images={:.2f} MiB jpg=20".format(
        app_bytes / 1024 / 1024,
        image_bytes / 1024 / 1024,
    ))


if __name__ == "__main__":
    main()
