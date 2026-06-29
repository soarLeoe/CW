#!/usr/bin/env python3
"""
OneNET Studio MQTT 诊断工具 (paho-mqtt v2 compatible)

自动尝试多种组合:
  - 签名格式: Studio新版(et\nmethod\nres\nversion) vs 旧版(res\net\nmethod)
  - res: 产品级 vs 设备级
  - method: sha1 / md5 / sha256
"""

import base64
import hmac
import hashlib
import urllib.parse
import time
import sys

try:
    import paho.mqtt.client as mqtt
except ImportError:
    print("需要安装: pip install paho-mqtt")
    sys.exit(1)

# ============================================================
PRODUCT_ID  = "NuuS3vUVz5"
DEVICE_NAME = "ESP32S3_HR_02"
ACCESS_KEY  = "QzZCNklSMmFuYWQ3ZHhISjVaTkJmR0JDck5LVHlvRVA="
BROKER      = "mqtts.heclouds.com"
PORT        = 1883
EXPIRE_DAYS = 365 * 5
# ============================================================

VERSION = "2018-10-31"


def make_token_studio(res, et, key_bytes, method):
    """Studio: et \n method \n res \n version"""
    sig = f"{et}\n{method}\n{res}\n{VERSION}"
    hm = {"md5": hashlib.md5, "sha1": hashlib.sha1, "sha256": hashlib.sha256}
    sign = base64.b64encode(
        hmac.new(key_bytes, sig.encode("utf-8"), hm[method]).digest()
    ).decode("utf-8")
    return (f"version={VERSION}"
            f"&res={urllib.parse.quote(res, safe='')}"
            f"&et={et}&method={method}"
            f"&sign={urllib.parse.quote(sign, safe='')}")


def make_token_legacy(res, et, key_bytes, method):
    """Legacy: res \n et \n method"""
    sig = f"{res}\n{et}\n{method}"
    hm = {"md5": hashlib.md5, "sha1": hashlib.sha1, "sha256": hashlib.sha256}
    sign = base64.b64encode(
        hmac.new(key_bytes, sig.encode("utf-8"), hm[method]).digest()
    ).decode("utf-8")
    return (f"version={VERSION}"
            f"&res={urllib.parse.quote(res, safe='')}"
            f"&et={et}&method={method}"
            f"&sign={urllib.parse.quote(sign, safe='')}")


def test_one(label, token):
    print(f"\n--- {label} ---")
    print(f"  token={token[:70]}...")

    result = {"ok": False}

    def on_connect(client, userdata, flags, reason_code, properties=None):
        rc_str = str(reason_code)
        # paho-mqtt v2: "Success" = rc 0, others = error
        if rc_str == "Success" or rc_str == "0":
            result["ok"] = True
            print(f"  >> SUCCESS! (rc={rc_str})")
        else:
            print(f"  >> FAIL (rc={rc_str})")
        client.disconnect()

    client = mqtt.Client(
        mqtt.CallbackAPIVersion.VERSION2,
        client_id=DEVICE_NAME,
        protocol=mqtt.MQTTv311,
    )
    client.username_pw_set(PRODUCT_ID, token)
    client.on_connect = on_connect

    try:
        client.connect(BROKER, PORT, keepalive=30)
        client.loop_start()
        time.sleep(3)
        client.loop_stop()
        try: client.disconnect()
        except: pass
    except Exception as e:
        print(f"  >> NET ERROR: {e}")

    return result["ok"]


def main():
    print("=" * 60)
    print("  OneNET Studio MQTT Diagnostic")
    print("=" * 60)
    print(f"  ProductID : {PRODUCT_ID}")
    print(f"  DeviceName: {DEVICE_NAME}")
    print(f"  AccessKey : {ACCESS_KEY}")

    key_b64 = base64.b64decode(ACCESS_KEY)
    print(f"  Key decoded: {len(key_b64)}B")

    et = str(int(time.time()) + EXPIRE_DAYS * 86400)
    res_p = f"products/{PRODUCT_ID}"
    res_d = f"products/{PRODUCT_ID}/devices/{DEVICE_NAME}"

    tests = [
        ("STUDIO product sha1",    make_token_studio(res_p, et, key_b64, "sha1")),
        ("STUDIO product md5",     make_token_studio(res_p, et, key_b64, "md5")),
        ("STUDIO product sha256",  make_token_studio(res_p, et, key_b64, "sha256")),
        ("STUDIO device sha1",     make_token_studio(res_d, et, key_b64, "sha1")),
        ("STUDIO device md5",      make_token_studio(res_d, et, key_b64, "md5")),
        ("LEGACY device sha1",     make_token_legacy(res_d, et, key_b64, "sha1")),
        ("LEGACY device md5",      make_token_legacy(res_d, et, key_b64, "md5")),
        ("LEGACY product sha1",    make_token_legacy(res_p, et, key_b64, "sha1")),
    ]

    for label, token in tests:
        ok = test_one(label, token)
        if ok:
            print(f"\n{'*'*60}")
            print(f"  WORKING: {label}")
            print(f"{'*'*60}")
            print(f"\n  TOKEN:\n  {token}")
            print(f"\n  Copy to onenet_mqtt.h ONENET_TOKEN")
            return
        time.sleep(1)

    print(f"\n{'!'*60}")
    print("  ALL FAILED - access_key is wrong")
    print(f"{'!'*60}")


if __name__ == "__main__":
    main()
