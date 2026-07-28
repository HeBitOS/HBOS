/**
 * @file p256.c
 * @brief NIST P-256 域/群运算 + ECDH（Montgomery 乘法，Jacobian 坐标）。
 *
 * 4 个 64 位肢体表示 256 位域元素（小端）。乘法用 __uint128_t 得 64x64→128，
 * Montgomery 约减（p 的 -p^{-1} mod 2^64 = 1，见下）。求逆走费马小定理
 * a^(p-2)。标量乘用简单 double-and-add（变时——客户端临时密钥，不做常时
 * 抗侧信道，对浏览器场景可接受）。全部经 openssl 主机端逐位对拍验证。
 */

#include "p256.h"

typedef unsigned long long u64;
typedef __uint128_t u128;

/* p = 2^256 - 2^224 + 2^192 + 2^96 - 1（小端肢体） */
static const u64 P[4] = {
    0xffffffffffffffffULL, 0x00000000ffffffffULL,
    0x0000000000000000ULL, 0xffffffff00000001ULL};
/* R^2 mod p（转 Montgomery 域用） */
static const u64 RR[4] = {
    0x0000000000000003ULL, 0xfffffffbffffffffULL,
    0xfffffffffffffffeULL, 0x00000004fffffffdULL};
/* 曲线参数 b（普通域，运行时转 Montgomery） */
static const u64 B[4] = {
    0x3bce3c3e27d2604bULL, 0x651d06b0cc53b0f6ULL,
    0xb3ebbd55769886bcULL, 0x5ac635d8aa3a93e7ULL};
/* 基点 G（普通域仿射，小端肢体） */
static const u64 GX[4] = {
    0xf4a13945d898c296ULL, 0x77037d812deb33a0ULL,
    0xf8bce6e563a440f2ULL, 0x6b17d1f2e12c4247ULL};
static const u64 GY[4] = {
    0xcbb6406837bf51f5ULL, 0x2bce33576b315eceULL,
    0x8ee7eb4a7c0f9e16ULL, 0x4fe342e2fe1a7f9bULL};

/* -p^{-1} mod 2^64：p[0] = -1 mod 2^64 → p^{-1}=-1 → -p^{-1}=1 */
#define P_INV 1ULL

static int fe_iszero(const u64 a[4]) { return (a[0] | a[1] | a[2] | a[3]) == 0; }
static void fe_set(u64 r[4], const u64 a[4]) { r[0]=a[0]; r[1]=a[1]; r[2]=a[2]; r[3]=a[3]; }
static void fe_zero(u64 r[4]) { r[0]=r[1]=r[2]=r[3]=0; }

/* r = a + b mod p */
static void fe_add(u64 r[4], const u64 a[4], const u64 b[4]) {
    u128 c = 0;
    u64 t[4];
    for (int i = 0; i < 4; i++) { c += (u128)a[i] + b[i]; t[i] = (u64)c; c >>= 64; }
    /* 若溢出或 >= p 则减 p */
    u128 borrow = 0;
    u64 s[4];
    for (int i = 0; i < 4; i++) { u128 d = (u128)t[i] - P[i] - borrow; s[i] = (u64)d; borrow = (d >> 64) & 1; }
    /* c(进位)=1 说明肯定 >=p；否则 borrow=0 说明 t>=p */
    if (c || !borrow) fe_set(r, s); else fe_set(r, t);
}

/* r = a - b mod p */
static void fe_sub(u64 r[4], const u64 a[4], const u64 b[4]) {
    u128 borrow = 0;
    u64 t[4];
    for (int i = 0; i < 4; i++) { u128 d = (u128)a[i] - b[i] - borrow; t[i] = (u64)d; borrow = (d >> 64) & 1; }
    if (borrow) { /* 借位 → 加回 p */
        u128 c = 0;
        for (int i = 0; i < 4; i++) { c += (u128)t[i] + P[i]; t[i] = (u64)c; c >>= 64; }
    }
    fe_set(r, t);
}

/* Montgomery 乘法：r = a*b*R^{-1} mod p（CIOS） */
static void fe_mont_mul(u64 r[4], const u64 a[4], const u64 b[4]) {
    u64 t[5] = {0,0,0,0,0};
    for (int i = 0; i < 4; i++) {
        /* t += a[i]*b */
        u128 c = 0;
        for (int j = 0; j < 4; j++) {
            c += (u128)a[i] * b[j] + t[j];
            t[j] = (u64)c; c >>= 64;
        }
        u128 sum = (u128)t[4] + c;
        t[4] = (u64)sum;
        u64 carry_hi = (u64)(sum >> 64);
        /* m = t[0]*p_inv mod 2^64；t += m*p，使 t[0]==0 */
        u64 m = t[0] * P_INV;
        c = (u128)m * P[0] + t[0];
        c >>= 64;
        for (int j = 1; j < 4; j++) {
            c += (u128)m * P[j] + t[j];
            t[j - 1] = (u64)c; c >>= 64;
        }
        u128 s2 = (u128)t[4] + c;
        t[3] = (u64)s2;
        t[4] = carry_hi + (u64)(s2 >> 64);
    }
    /* 条件减 p */
    u128 borrow = 0;
    u64 s[4];
    for (int i = 0; i < 4; i++) { u128 d = (u128)t[i] - P[i] - borrow; s[i] = (u64)d; borrow = (d >> 64) & 1; }
    if (t[4] || !borrow) fe_set(r, s); else { r[0]=t[0]; r[1]=t[1]; r[2]=t[2]; r[3]=t[3]; }
}

static void fe_mont_sqr(u64 r[4], const u64 a[4]) { fe_mont_mul(r, a, a); }

/* 转入/转出 Montgomery 域 */
static void fe_to_mont(u64 r[4], const u64 a[4]) { fe_mont_mul(r, a, RR); }
static void fe_from_mont(u64 r[4], const u64 a[4]) {
    u64 one[4] = {1,0,0,0};
    fe_mont_mul(r, a, one);
}

/* Montgomery 域求逆：a^(p-2)。p-2 的肢体： */
static void fe_mont_inv(u64 r[4], const u64 a[4]) {
    static const u64 PM2[4] = {
        0xfffffffffffffffdULL, 0x00000000ffffffffULL,
        0x0000000000000000ULL, 0xffffffff00000001ULL};
    u64 res[4]; u64 one[4] = {1,0,0,0};
    fe_to_mont(res, one); /* res = 1 in mont */
    u64 base[4]; fe_set(base, a);
    for (int i = 0; i < 256; i++) {
        int bit = (PM2[i >> 6] >> (i & 63)) & 1;
        if (bit) fe_mont_mul(res, res, base);
        fe_mont_sqr(base, base);
    }
    fe_set(r, res);
}

/* ── Jacobian 点 (X:Y:Z)，坐标均为 Montgomery 域 ── */
typedef struct { u64 x[4], y[4], z[4]; } jac_t;

static int jac_is_inf(const jac_t *p) { return fe_iszero(p->z); }

/* 点倍：RFC/标准公式（a=-3） */
static void jac_double(jac_t *r, const jac_t *p) {
    if (jac_is_inf(p)) { *r = *p; return; }
    u64 t1[4], t2[4], t3[4], t4[4];
    fe_mont_sqr(t1, p->z);           /* z^2 */
    fe_sub(t2, p->x, t1);            /* x - z^2 */
    fe_add(t1, p->x, t1);            /* x + z^2 */
    fe_mont_mul(t2, t2, t1);         /* (x-z^2)(x+z^2) */
    fe_add(t3, t2, t2); fe_add(t2, t3, t2); /* M = 3*(...) */
    fe_add(t3, p->y, p->y);          /* 2y */
    fe_mont_mul(r->z, t3, p->z);     /* Z' = 2y*z */
    fe_mont_sqr(t3, t3);             /* 4y^2 */
    fe_mont_mul(t4, t3, p->x);       /* S = 4x*y^2 */
    fe_mont_sqr(t3, t3);             /* 16y^4 */
    /* Y' 用的 8y^4 = 16y^4/2 */
    u64 half[4];
    /* 除以 2 mod p：偶数右移，奇数加 p 再右移 */
    fe_set(half, t3);
    if (half[0] & 1) {
        u128 c = 0; for (int i=0;i<4;i++){ c += (u128)half[i] + P[i]; half[i]=(u64)c; c>>=64; }
        /* 带进位的右移 */
        u64 top = (u64)c;
        for (int i=0;i<3;i++) half[i] = (half[i]>>1) | (half[i+1]<<63);
        half[3] = (half[3]>>1) | (top<<63);
    } else {
        for (int i=0;i<3;i++) half[i] = (half[i]>>1) | (half[i+1]<<63);
        half[3] = half[3]>>1;
    }
    fe_mont_sqr(r->x, t2);           /* M^2 */
    fe_add(t1, t4, t4);              /* 2S */
    fe_sub(r->x, r->x, t1);          /* X' = M^2 - 2S */
    fe_sub(t1, t4, r->x);            /* S - X' */
    fe_mont_mul(t1, t2, t1);         /* M*(S-X') */
    fe_sub(r->y, t1, half);          /* Y' = M*(S-X') - 8y^4 */
}

/* 点加：p (Jacobian) + q (仿射 Montgomery, z=1)，标准混合加 */
static void jac_add_affine(jac_t *r, const jac_t *p, const u64 qx[4], const u64 qy[4]) {
    if (jac_is_inf(p)) {
        fe_set(r->x, qx); fe_set(r->y, qy);
        u64 one[4] = {1,0,0,0}; fe_to_mont(r->z, one);
        return;
    }
    u64 z2[4], u2[4], s2[4], h[4], rr[4], h2[4], h3[4], t[4];
    fe_mont_sqr(z2, p->z);
    fe_mont_mul(u2, qx, z2);         /* U2 = qx*z^2 */
    fe_mont_mul(s2, qy, z2);
    fe_mont_mul(s2, s2, p->z);       /* S2 = qy*z^3 */
    fe_sub(h, u2, p->x);             /* H = U2 - X1 */
    fe_sub(rr, s2, p->y);            /* R = S2 - Y1 */
    if (fe_iszero(h)) {
        if (fe_iszero(rr)) { jac_double(r, p); return; }
        fe_zero(r->z); return;       /* 无穷远 */
    }
    fe_mont_sqr(h2, h);
    fe_mont_mul(h3, h2, h);
    fe_mont_mul(t, p->x, h2);        /* X1*H^2 */
    fe_mont_sqr(r->x, rr);
    fe_sub(r->x, r->x, h3);
    u64 t2[4]; fe_add(t2, t, t);
    fe_sub(r->x, r->x, t2);          /* X3 = R^2 - H^3 - 2*X1*H^2 */
    fe_sub(t, t, r->x);
    fe_mont_mul(t, rr, t);
    fe_mont_mul(h3, h3, p->y);
    fe_sub(r->y, t, h3);             /* Y3 = R*(X1*H^2 - X3) - Y1*H^3 */
    fe_mont_mul(r->z, p->z, h);      /* Z3 = Z1*H */
}

/* 标量乘 k*P（P 仿射 Montgomery）；k 为 32 字节大端 */
static void scalar_mul(jac_t *r, const u64 px[4], const u64 py[4], const uint8_t k[32]) {
    fe_zero(r->x); fe_zero(r->y); fe_zero(r->z); /* 无穷远 */
    for (int i = 0; i < 256; i++) {
        jac_double(r, r);
        int byte = k[i >> 3];
        int bit = (byte >> (7 - (i & 7))) & 1;
        if (bit) jac_add_affine(r, r, px, py);
    }
}

/* Jacobian → 仿射（输出普通域），返回是否有效 */
static int jac_to_affine(u64 ox[4], u64 oy[4], const jac_t *p) {
    if (jac_is_inf(p)) return 0;
    u64 zinv[4], zinv2[4], zinv3[4];
    fe_mont_inv(zinv, p->z);
    fe_mont_sqr(zinv2, zinv);
    fe_mont_mul(zinv3, zinv2, zinv);
    u64 x[4], y[4];
    fe_mont_mul(x, p->x, zinv2);
    fe_mont_mul(y, p->y, zinv3);
    fe_from_mont(ox, x);
    fe_from_mont(oy, y);
    return 1;
}

/* 大端字节 <-> 肢体 */
static void be_to_fe(u64 r[4], const uint8_t b[32]) {
    for (int i = 0; i < 4; i++) {
        u64 v = 0;
        for (int j = 0; j < 8; j++) v = (v << 8) | b[i * 8 + j];
        r[3 - i] = v;
    }
}
static void fe_to_be(uint8_t b[32], const u64 a[4]) {
    for (int i = 0; i < 4; i++) {
        u64 v = a[3 - i];
        for (int j = 0; j < 8; j++) b[i * 8 + j] = (uint8_t)(v >> (56 - j * 8));
    }
}

/* 校验点在曲线上：y^2 == x^3 - 3x + b（普通仿射输入） */
static int on_curve(const u64 x[4], const u64 y[4]) {
    u64 xm[4], ym[4], t[4], rhs[4], three[4] = {3,0,0,0}, bm[4], threem[4];
    fe_to_mont(xm, x); fe_to_mont(ym, y);
    fe_to_mont(bm, B); fe_to_mont(threem, three);
    fe_mont_sqr(t, xm);
    fe_mont_mul(rhs, t, xm);         /* x^3 */
    fe_mont_mul(t, threem, xm);      /* 3x */
    fe_sub(rhs, rhs, t);             /* x^3 - 3x */
    fe_add(rhs, rhs, bm);            /* + b */
    fe_mont_sqr(t, ym);              /* y^2 */
    return t[0]==rhs[0] && t[1]==rhs[1] && t[2]==rhs[2] && t[3]==rhs[3];
}

void p256_public_key(uint8_t out_xy[64], const uint8_t priv[32]) {
    u64 gx[4], gy[4];
    fe_to_mont(gx, GX); fe_to_mont(gy, GY);
    jac_t r;
    scalar_mul(&r, gx, gy, priv);
    u64 ox[4], oy[4];
    if (!jac_to_affine(ox, oy, &r)) { for (int i=0;i<64;i++) out_xy[i]=0; return; }
    fe_to_be(out_xy, ox);
    fe_to_be(out_xy + 32, oy);
}

int p256_ecdh(uint8_t out_x[32], const uint8_t priv[32], const uint8_t peer_xy[64]) {
    for (int i = 0; i < 32; i++) out_x[i] = 0;
    u64 px[4], py[4];
    be_to_fe(px, peer_xy);
    be_to_fe(py, peer_xy + 32);
    /* 点必须在曲线上且非无穷远 */
    if (fe_iszero(px) && fe_iszero(py)) return -1;
    if (!on_curve(px, py)) return -1;
    u64 pxm[4], pym[4];
    fe_to_mont(pxm, px); fe_to_mont(pym, py);
    jac_t r;
    scalar_mul(&r, pxm, pym, priv);
    u64 ox[4], oy[4];
    if (!jac_to_affine(ox, oy, &r)) return -1;
    fe_to_be(out_x, ox);
    return 0;
}
