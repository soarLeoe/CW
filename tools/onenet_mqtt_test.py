#!/usr/bin/env python3
"""
OneNET MQTT 连接测试工具 (paho-mqtt v2 compatible)

直接用 PC 连接 OneNET broker，自动尝试三种签名方法(md5/sha1/sha256)，
快速定位 Token 问题，不用每次烧录 ESP32。

用法: python onenet_mqtt_test.py
依赖: pip install paho-mqtt
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
#  CONFIG
# ============================================================
PRODUCT_ID  = "5hbIwupOGf"
DEVICE_NAME = "ESP32S3_HR_01"
ACCESS_KEY  = "QzhCRnhtQ2NnMW9HcFVOaXh5elQwdmFMdjlRWE9NbzA="
BROKER      = "mqtts.heclouds.com"
PORT        = 1883
EXPIRE_DAYS = 365 * 5
# ============================================================


def make_token(product_id, device_name, access_key, expire_seconds, method="md5"):
    res = f"products/{product_id}/devices/{device_name}"
    et = str(int(time.time()) + expire_seconds)
    string_for_signature = f"{res}\n{et}\n{method}"
    key_bytes = base64.b64decode(access_key)

    hash_map = {"md5": hashlib.md5, "sha1": hashlib.sha1, "sha256": hashlib.sha256}
    hasher = hash_map[method]
    sign_bytes = hmac.new(key_bytes,
                          string_for_signature.encode("utf-8"),
                          hasher).digest()
    sign = base64.b64encode(sign_bytes).decode("utf-8")

    token = (
        "version=2018-10-31"
        f"&res={urllib.parse.quote(res, safe='')}"
        f"&et={et}"
        f"&method={method}"
        f"&sign={urllib.parse.quote(sign, safe='')}"
    )
    return token


def test_connection(token, method_label):
    print(f"\n{'='*60}")
    print(f"  {method_label}")
    print(f"{'='*60}")

    result = {"connected": False, "rc": None}

    def on_connect(client, userdata, flags, reason_code, properties=None):
        rc_val = int(reason_code) if hasattr(reason_code, '__int__') else str(reason_code)
        result["rc"] = rc_val
        if rc_val == 0:
            result["connected"] = True
            print(f"  >> SUCCESS! Connected to OneNET (rc=0)")
        else:
            codes = {1: "wrong protocol version", 2: "client ID rejected",
                     3: "broker unavailable", 4: "bad username/password",
                     5: "not authorized"}
            desc = codes.get(rc_val, str(reason_code))
            print(f"  >> FAILED (rc={rc_val}): {desc}")
        client.disconnect()

    client = mqtt.Client(
        mqtt.CallbackAPIVersion.VERSION2,
        client_id=DEVICE_NAME,
        protocol=mqtt.MQTTv311,
    )
    client.username_pw_set(PRODUCT_ID, token)
    client.on_connect = on_connect

    print(f"  ClientID : {DEVICE_NAME}")
    print(f"  Username : {PRODUCT_ID}")
    print(f"  Token    : {token[:70]}...")

    try:
        client.connect(BROKER, PORT, keepalive=30)
        client.loop_start()
        time.sleep(4)
        client.loop_stop()
        try:
            client.disconnect()
        except:
            pass
    except Exception as e:
        print(f"  >> Network error: {e}")
        result["rc"] = -1

    return result


def main():
    print("=" * 60)
    print("  OneNET MQTT Connection Test")
    print("=" * 60)
    print(f"  Broker   : {BROKER}:{PORT}")
    print(f"  Product  : {PRODUCT_ID}")
    print(f"  Device   : {DEVICE_NAME}")
    print(f"  Key      : {ACCESS_KEY}")

    try:
        decoded = base64.b64decode(ACCESS_KEY)
        print(f"  Key dec  : {len(decoded)} bytes")
        try:
            print(f"  Key ascii: {decoded.decode('ascii')}")
        except:
            print(f"  Key hex  : {decoded.hex()}")
    except Exception as e:
        print(f"  !! access_key base64 decode failed: {e}")
        return

    methods = ["md5", "sha1", "sha256"]
    success_method = None

    for method in methods:
        token = make_token(PRODUCT_ID, DEVICE_NAME, ACCESS_KEY,
                           EXPIRE_DAYS * 86400, method)
        result = test_connection(token, f"Method = {method}")
        if result["connected"]:
            success_method = method
            print(f"\n{'*'*60}")
            print(f"  SUCCESS with method={method}")
            print(f"{'*'*60}")
            print("\nCopy this token to onenet_mqtt.h ONENET_TOKEN:")
            print("-" * 60)
            print(token)
            print("-" * 60)
            break
        time.sleep(1)

    if not success_method:
        print(f"\n{'!'*60}")
        print("  All 3 methods FAILED!")
        print(f"{'!'*60}")
        print()
        print("Checklist:")
        print("  1. access_key: copy from 设备管理->设备详情->设备密钥")
        print("     (NOT 产品开发->产品密钥)")
        print("  2. Device must be registered and active on OneNET")
        print("  3. OneNET Studio (new) vs old multi-protocol:")
        print("     Studio PID = alphanumeric (5hbIwupOGf)")
        print("     Old PID = numeric only (12345678)")
        print("  4. DeviceName case-sensitive match")


if __name__ == "__main__":
    main()
