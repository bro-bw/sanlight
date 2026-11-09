# -*- coding: utf-8 -*-
"""
SanLight 离线算法验证: 用合成球体场景跑与 SanLight.cpp 完全一致的
光照内核, 输出预览条 (build/preview_*.png)。

用法:  python tools/preview.py
"""
import math
import os
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.normpath(os.path.join(HERE, "..", "build"))
os.makedirs(OUT, exist_ok=True)

W = H = 512


def make_scene():
    """球体 + 背景平面: 返回 (normal_map, nearness_map)"""
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float64)
    cx, cy, r = W * 0.48, H * 0.45, W * 0.30
    dx = (xx - cx) / r
    dy = (yy - cy) / r
    d2 = dx * dx + dy * dy
    inside = d2 <= 1.0

    nz = np.sqrt(np.clip(1.0 - d2, 0.0, 1.0))
    # 贴图编码: R=+x右, G=+y上 (OpenGL), B=+z朝镜头
    nx_map = np.where(inside, dx, 0.0)
    ny_map = np.where(inside, -dy, 0.0)
    nz_map = np.where(inside, nz, 1.0)
    length = np.sqrt(nx_map**2 + ny_map**2 + nz_map**2)
    normal = np.stack([(nx_map / length + 1) * 0.5,
                       (ny_map / length + 1) * 0.5,
                       (nz_map / length + 1) * 0.5], axis=-1)

    # 羽化边缘的近度图: 球=1, 背景=0.15
    edge = np.clip((1.0 - np.sqrt(d2)) / 0.05, 0.0, 1.0)
    nearness = np.where(inside, 0.15 + (1.0 - 0.15) * edge, 0.15)

    # 原图: 暖肤色球 + 深灰背景 (示意动漫角色)
    img = np.zeros((H, W, 4), np.float64)
    img[..., 0] = 0.86
    img[..., 1] = 0.72
    img[..., 2] = 0.62
    img[..., 3] = np.clip(edge * 1.2, 0, 1)
    img[..., 3][~inside] = 0.0
    return normal, nearness, img


def shade(normal, nearness, src, p, view=1):
    """与 SanLight.cpp 同款内核 (矢量化)"""
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float64)

    def get_normal():
        nx = normal[..., 0] * 2 - 1
        ny = normal[..., 1] * 2 - 1
        nz = normal[..., 2] * 2 - 1
        if p.get("normalYUp", True):
            ny = -ny
        ln = np.sqrt(nx**2 + ny**2 + nz**2) + 1e-9
        return nx / ln, ny / ln, nz / ln

    Nx, Ny, Nz = get_normal()
    z = p["depthRange"] * (1.0 - nearness)

    lx = p["lightX"] - xx
    ly = p["lightY"] - yy
    lz = p["lightZ"] - z
    dist = np.sqrt(lx**2 + ly**2 + lz**2) + 1e-9

    if p["falloff"] > 0.5:
        q = dist / p["falloff"]
        atten = 1.0 / (1.0 + q * q)
    else:
        atten = 1.0

    ndl = (Nx * lx + Ny * ly + Nz * lz) / dist
    w = p["softness"]
    if w > 0.0005:
        t = np.clip((ndl + w) / (2 * w), 0, 1)
        diff = t * t * (3 - 2 * t)
    else:
        diff = np.clip(ndl, 0, 1)

    ix, iy, iz = lx / dist, ly / dist, lz / dist
    hl = np.sqrt(ix**2 + iy**2 + (iz + 1)**2) + 1e-9
    ndh = Nx * ix + Ny * iy + Nz * (iz + 1) / hl
    spec = np.where(ndh > 0, ndh, 0) ** p["shininess"]

    rim = (1.0 - np.abs(Nz)) ** 3

    kI = p["intensity"]
    diffTerm = kI * atten * diff * p["diffuse"]
    specTerm = kI * atten * spec * p["specular"]
    rimTerm = p["rim"] * rim

    if view >= 2:
        if view == 2:
            g = np.clip(p["ambient"] + diffTerm + specTerm + rimTerm, 0, 1)
        elif view == 3:
            g = np.clip(diffTerm, 0, 1)
        elif view == 4:
            g = np.clip(specTerm, 0, 1)
        elif view == 5:
            g = np.clip(rimTerm, 0, 1)
        elif view == 6:
            return np.dstack([normal[..., 0], normal[..., 1], normal[..., 2],
                              src[..., 3]])
        else:
            g = np.clip(nearness, 0, 1)
        return np.dstack([g, g, g, src[..., 3]])

    L = p["lightColor"]
    amb = p["ambient"]
    shade = np.stack([amb + L[0] * (diffTerm + specTerm) + L[0] * rimTerm,
                      amb + L[1] * (diffTerm + specTerm) + L[1] * rimTerm,
                      amb + L[2] * (diffTerm + specTerm) + L[2] * rimTerm], axis=-1)

    # 色阶 (AE Levels)
    rng = max(p["lvInW"] - p["lvInB"], 0.004)
    t = np.clip((shade - p["lvInB"]) / rng, 0, 1)
    if abs(p["lvGamma"] - 1) > 1e-4:
        t = t ** (1.0 / p["lvGamma"])
    shade = p["lvOutB"] + (p["lvOutW"] - p["lvOutB"]) * t
    shade = np.clip(shade, 0, 1)

    a = p["apply"]
    if a == 2:      # 正片叠底
        outv = src[..., :3] * shade
    elif a == 3:    # 叠加
        s = src[..., :3]
        outv = np.where(s < 0.5, 2 * s * shade, 1 - 2 * (1 - s) * (1 - shade))
    elif a == 4:    # 柔光
        s = src[..., :3]
        outv = np.where(shade <= 0.5,
                        s - (1 - 2 * shade) * s * (1 - s),
                        s + (2 * shade - 1) * (np.sqrt(np.clip(s, 0, 1)) - s))
    elif a == 5:    # 相加 (只加光)
        outv = s + np.clip(shade - amb, 0, None)
    else:           # 仅光影
        outv = shade
    outv = src[..., :3] + (outv - src[..., :3]) * p["opacity"]
    return np.dstack([np.clip(outv, 0, 1), src[..., 3]])


def to8(img):
    return (np.clip(img, 0, 1) * 255 + 0.5).astype(np.uint8)


def label(strip, text):
    from PIL import ImageDraw
    im = Image.fromarray(strip)
    d = ImageDraw.Draw(im)
    d.rectangle([0, 0, strip.shape[1] - 1, 20], fill=(20, 20, 20))
    d.text((6, 4), text, fill=(240, 240, 240))
    return np.array(im)


def main():
    normal, nearness, src = make_scene()

    base = dict(lightX=W * 0.30, lightY=H * 0.22, lightZ=700.0,
                lightColor=(1.0, 240 / 255, 220 / 255),
                intensity=1.2, falloff=2500.0, ambient=0.30,
                diffuse=1.0, specular=0.30, shininess=24, rim=0.0,
                softness=0.032, depthRange=1000.0,
                lvInB=0.0, lvInW=1.0, lvGamma=1.0, lvOutB=0.0, lvOutW=1.0,
                apply=2, opacity=1.0, normalYUp=True)

    # 1) 开箱默认 (正片叠底, 柔和光影)
    d1 = shade(normal, nearness, src, base)
    # 2) 图一式硬边: 色阶压二分 (输入黑35 输入白150) + 无过渡
    hard = dict(base); hard.update(lvInB=35 / 255, lvInW=150 / 255, softness=0.0)
    d2 = shade(normal, nearness, src, hard)
    # 3) 仅光影图
    lo = dict(base); lo["apply"] = 1
    d3 = shade(normal, nearness, src, lo)
    # 4) 点光源移到右侧 + 强衰减 (验证 XYZ/衰减)
    right = dict(base); right.update(lightX=W * 0.85, falloff=700.0, intensity=2.0)
    d4 = shade(normal, nearness, src, right)
    # 5) 背光 (光源Z=-400, 在画面后方 -> 应大面积变暗)
    back = dict(base); back.update(lightZ=-400.0)
    d5 = shade(normal, nearness, src, back)
    # 6) 高光+轮廓光
    rim = dict(base); rim.update(specular=1.5, rim=60, shininess=64)
    d6 = shade(normal, nearness, src, rim)

    nm = np.dstack([normal, np.ones((H, W))])
    dp = np.dstack([nearness] * 3 + [np.ones((H, W))])

    rows = [
        (to8(np.dstack([src[..., :3], src[..., 3]])), "0 source"),
        (to8(nm), "1 normal map (G up)"),
        (to8(dp), "2 depth (white=near)"),
        (to8(d1), "3 default: multiply, soft"),
        (to8(d2), "4 levels crush 35/150 = hard edge"),
        (to8(d3), "5 lighting only"),
        (to8(d4), "6 light right, falloff 700, int 200%"),
        (to8(d5), "7 light behind (Z=-400)"),
        (to8(d6), "8 spec+rim"),
    ]
    strip = np.concatenate([label(*r) for r in rows], axis=0)
    out_path = os.path.join(OUT, "preview_strip.png")
    Image.fromarray(strip).save(out_path)
    print("saved", out_path)

    # 数值断言: 硬边模式下球面应有明显二分 (存在亮区与暗区), 背光应整体变暗
    sphere = src[..., 3] > 0.5
    lum = d2[..., :3].mean(-1)[sphere]
    assert lum.max() > 0.7 and lum.min() < 0.45, "hard mode missing terminator"
    lum5 = d5[..., :3].mean(-1)[sphere]
    lum3 = d1[..., :3].mean(-1)[sphere]
    assert lum5.mean() < lum3.mean(), "back light should darken"
    print("assertions OK  (hard min=%.2f max=%.2f | back=%.2f front=%.2f)"
          % (lum.min(), lum.max(), lum5.mean(), lum3.mean()))


if __name__ == "__main__":
    main()
