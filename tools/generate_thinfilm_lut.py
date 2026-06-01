from __future__ import annotations

import math
from pathlib import Path

from PIL import Image


WIDTH = 256
HEIGHT = 256
MIN_THICKNESS_NM = 100.0
MAX_THICKNESS_NM = 900.0
WAVELENGTH_MIN_NM = 380
WAVELENGTH_MAX_NM = 780
NUM_SPECTRAL_BANDS = 64
SOAP_IOR = 1.33
BUBBLE_INNER_IOR = 1.0
EXPOSURE = 1.0
SATURATION = 0.85


def x_fit_1931(wavelength_nm: float) -> float:
    t1 = (wavelength_nm - 442.0) * (0.0624 if wavelength_nm < 442.0 else 0.0374)
    t2 = (wavelength_nm - 599.8) * (0.0264 if wavelength_nm < 599.8 else 0.0323)
    t3 = (wavelength_nm - 501.1) * (0.0490 if wavelength_nm < 501.1 else 0.0382)
    return (
        0.362 * math.exp(-0.5 * t1 * t1)
        + 1.056 * math.exp(-0.5 * t2 * t2)
        - 0.065 * math.exp(-0.5 * t3 * t3)
    )


def y_fit_1931(wavelength_nm: float) -> float:
    t1 = (wavelength_nm - 568.8) * (0.0213 if wavelength_nm < 568.8 else 0.0247)
    t2 = (wavelength_nm - 530.9) * (0.0613 if wavelength_nm < 530.9 else 0.0322)
    return 0.821 * math.exp(-0.5 * t1 * t1) + 0.286 * math.exp(-0.5 * t2 * t2)


def z_fit_1931(wavelength_nm: float) -> float:
    t1 = (wavelength_nm - 437.0) * (0.0845 if wavelength_nm < 437.0 else 0.0278)
    t2 = (wavelength_nm - 459.0) * (0.0385 if wavelength_nm < 459.0 else 0.0725)
    return 1.217 * math.exp(-0.5 * t1 * t1) + 0.681 * math.exp(-0.5 * t2 * t2)


def fresnel_reflectance(
    eta_a: float,
    eta_b: float,
    cos_a: float,
    cos_b: float,
) -> tuple[float, float, float, float]:
    sin_a_sq = max(0.0, 1.0 - cos_a * cos_a)
    eta_ratio = eta_a / eta_b

    if eta_ratio * eta_ratio * sin_a_sq > 1.0:
        eta_ratio_sq = eta_ratio * eta_ratio
        phase_p = 2.0 * math.atan(
            -eta_ratio_sq * math.sqrt(sin_a_sq - 1.0 / eta_ratio_sq) / cos_a
        )
        phase_s = 2.0 * math.atan(
            -math.sqrt(sin_a_sq - 1.0 / eta_ratio_sq) / cos_a
        )
        return 1.0, 1.0, phase_p, phase_s

    r_p = (eta_b * cos_a - eta_a * cos_b) / (eta_b * cos_a + eta_a * cos_b)
    r_s = (eta_a * cos_a - eta_b * cos_b) / (eta_a * cos_a + eta_b * cos_b)
    phase_p = math.pi if r_p < 0.0 else 0.0
    phase_s = math.pi if r_s < 0.0 else 0.0
    return r_p * r_p, r_s * r_s, phase_p, phase_s


def belcour_reflectance(cos_theta: float, thickness_nm: float, wavelength_nm: float) -> float:
    eta_film = SOAP_IOR
    eta_base = BUBBLE_INNER_IOR

    cos_i = max(0.001, min(0.999, cos_theta))
    refr_ratio_sq = 1.0 / (eta_film * eta_film)
    refr_ratio_sq_base = (eta_film * eta_film) / (eta_base * eta_base)
    cos_t = math.sqrt(max(0.0, 1.0 - refr_ratio_sq * (1.0 - cos_i * cos_i)))
    cos_t2 = math.sqrt(max(0.0, 1.0 - refr_ratio_sq_base * (1.0 - cos_t * cos_t)))

    path_diff = 2.0 * eta_film * thickness_nm * cos_t
    delta_phase = 2.0 * math.pi * path_diff / wavelength_nm

    r12_p, r12_s, phi12_p, phi12_s = fresnel_reflectance(1.0, eta_film, cos_i, cos_t)
    r23_p, r23_s, phi23_p, phi23_s = fresnel_reflectance(eta_film, eta_base, cos_t, cos_t2)
    t12_p = 1.0 - r12_p
    t12_s = 1.0 - r12_s
    phi21_p = math.pi - phi12_p
    phi21_s = math.pi - phi12_s

    def polarized(r12: float, t12: float, r23: float, phi23: float, phi21: float) -> float:
        r_bi = math.sqrt(max(0.0, r23 * r12))
        r_bi_sq = r_bi * r_bi
        r_star = (t12 * t12 * r23) / max(1.0e-5, 1.0 - r23 * r12)
        r_12_star = r12 + r_star
        r_star_t_tot = r_star - abs(t12)
        cos_phi = math.cos(delta_phase + phi23 + phi21)
        denom = max(1.0e-5, 1.0 - 2.0 * r_bi * cos_phi + r_bi_sq)
        value = r_12_star + 2.0 * (r_bi * cos_phi - r_bi_sq) / denom * r_star_t_tot
        return max(0.0, min(1.0, value))

    val_p = polarized(r12_p, t12_p, r23_p, phi23_p, phi21_p)
    val_s = polarized(r12_s, t12_s, r23_s, phi23_s, phi21_s)
    return 0.5 * (val_p + val_s)


def xyz_to_srgb(x: float, y: float, z: float) -> tuple[float, float, float]:
    r = 3.2406 * x - 1.5372 * y - 0.4986 * z
    g = -0.9689 * x + 1.8758 * y + 0.0415 * z
    b = 0.0557 * x - 0.2040 * y + 1.0570 * z
    return r, g, b


def saturate(rgb: tuple[float, float, float], amount: float) -> tuple[float, float, float]:
    r, g, b = rgb
    luma = 0.2126 * r + 0.7152 * g + 0.0722 * b
    return (
        luma + (r - luma) * amount,
        luma + (g - luma) * amount,
        luma + (b - luma) * amount,
    )


def compute_rgb(cos_theta: float, thickness_nm: float) -> tuple[int, int, int]:
    x = y = z = 0.0
    tot_x = tot_y = tot_z = 0.0
    wave_range = WAVELENGTH_MAX_NM - WAVELENGTH_MIN_NM
    for band in range(NUM_SPECTRAL_BANDS):
        wavelength = WAVELENGTH_MIN_NM + band / (NUM_SPECTRAL_BANDS - 1) * wave_range
        cmf_x = x_fit_1931(wavelength)
        cmf_y = y_fit_1931(wavelength)
        cmf_z = z_fit_1931(wavelength)
        reflectance = belcour_reflectance(cos_theta, thickness_nm, float(wavelength))
        x += reflectance * cmf_x
        y += reflectance * cmf_y
        z += reflectance * cmf_z
        tot_x += cmf_x
        tot_y += cmf_y
        tot_z += cmf_z

    x /= tot_x
    y /= tot_y
    z /= tot_z
    rgb = xyz_to_srgb(x * EXPOSURE, y * EXPOSURE, z * EXPOSURE)
    rgb = saturate(rgb, SATURATION)
    # Match the reference implementation: store sqrt(linear RGB) in the LUT
    # and square the sampled value in the shader.
    return tuple(int(round(math.sqrt(max(0.0, min(1.0, channel))) * 255.0)) for channel in rgb)


def main() -> None:
    out_path = Path(__file__).resolve().parents[1] / "windows" / "assets" / "lut" / "thinfilm_belcour_bubble.png"
    out_path.parent.mkdir(parents=True, exist_ok=True)

    image = Image.new("RGB", (WIDTH, HEIGHT))
    pixels = image.load()
    for y in range(HEIGHT):
        thickness_t = y / (HEIGHT - 1)
        thickness_nm = MIN_THICKNESS_NM + thickness_t * (MAX_THICKNESS_NM - MIN_THICKNESS_NM)
        for x in range(WIDTH):
            cos_theta = max(0.001, x / (WIDTH - 1))
            pixels[x, y] = compute_rgb(cos_theta, thickness_nm)

    image.save(out_path)
    print(f"Wrote {out_path}")


if __name__ == "__main__":
    main()
