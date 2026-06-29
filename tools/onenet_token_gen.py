#!/usr/bin/env python3
"""
OneNET Studio Token 生成工具 (version=2018-10-31)

OneNET Studio 新版签名算法:
  StringForSignature = et + "\\n" + method + "\\n" + res + "\\n" + version
  res = "products/{ProductID}"           (产品级, 不含设备名!)
  key = 产品 access_key (base64)
  Sign = base64(HMAC-{method}(base64_decode(key), StringForSignature))
  Token = "version=2018-10-31&res={url(res)}&et={et}&method={method}&sign={url(Sign)}"

MQTT 连接参数:
  ClientID = 设备名称
  Username = 产品ID
  Password = Token

用法:
  1. 填 CONFIG 区的 ProductID 和产品 access_key
  2. python onenet_token_gen.py
  3. 把输出的 Token 粘进 onenet_mqtt.h 的 ONENET_TOKEN 宏
"""

import base64
import hmac
import hashlib
import urllib.parse
import time

# ============================================================
#  CONFIG
#  ProductID:  产品开发 -> 产品详情 -> 产品ID
#  ACCESS_KEY: 产品开发 -> 产品详情 -> access_key (产品级!)
# ============================================================
PRODUCT_ID  = "5hbIwupOGf"
ACCESS_KEY  = "QzhCRnhtQ2NnMW9HcFVOaXh5elQwdmFMdjlRWE9NbzA="
EXPIRE_DAYS = 365 * 5
# ============================================================


def make_token(product_id: str, access_key: str,
               expire_seconds: int, method: str = "sha1"):
    """OneNET Studio token generation."""
    version = "2018-10-31"
    res = f"products/{product_id}"
    et = str(int(time.time()) + expire_seconds)

    # Studio 新版签名串: et \n method \n res \n version
    string_for_signature = f"{et}\n{method}\n{res}\n{version}"

    key_bytes = base64.b64decode(access_key)

    hash_map = {"md5": hashlib.md5, "sha1": hashlib.sha1, "sha256": hashlib.sha256}
    sign_bytes = hmac.new(key_bytes,
                          string_for_signature.encode("utf-8"),
                          hash_map[method]).digest()
    sign = base64.b64encode(sign_bytes).decode("utf-8")

    token = (
        f"version={version}"
        f"&res={urllib.parse.quote(res, safe='')}"
        f"&et={et}"
        f"&method={method}"
        f"&sign={urllib.parse.quote(sign, safe='')}"
    )
    return token, res, et, sign


def main():
    token, res, et, sign = make_token(
        PRODUCT_ID, ACCESS_KEY, EXPIRE_DAYS * 86400, method="sha1")

    print("=" * 70)
    print("OneNET Studio MQTT Token")
    print("=" * 70)
    print(f"Broker    : mqtts.heclouds.com:1883")
    print(f"ClientID  : (设备名, 见 onenet_mqtt.h: ONENET_DEVICE_NAME)")
    print(f"Username  : {PRODUCT_ID}  (产品ID)")
    print(f"Password  : (见下方 Token)")
    print()
    print(f"res       : {res}  (产品级)")
    print(f"method    : sha1")
    print(f"et        : {et}")
    print(f"sign      : {sign}")
    print()
    print("Token (整串复制到 ONENET_TOKEN 宏):")
    print("-" * 70)
    print(token)
    print("-" * 70)

    days_left = (int(et) - int(time.time())) // 86400
    print(f"\n>> Token 有效, 剩余 {days_left} 天")


if __name__ == "__main__":
    main()
