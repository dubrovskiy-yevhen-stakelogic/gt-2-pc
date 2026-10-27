#pragma once
// GeometricFlexibleCallBack, CORE US1.01 SHA1 30031137ce494323816bb707b90d3ec0d07937d2.
// 0042c970..0042cd60: EE matrix stack, distinct from the VU render callback.
#include <array>
#include <stdexcept>
#include "ee/fpu.h"
#include "game/math/libm.h"
#include "game/math/quat.h"
#include "game/math/vu0_matrix.h"
#include <vector>

namespace gt4::game {
struct MotionTransform {
    using Matrix = std::array<uint32_t,16>;
    std::array<Matrix,16> stack{};
    size_t depth = 0;
    MotionTransform() { for (size_t i = 0; i < 4; ++i) stack[0][5*i] = 0x3f800000; }
    const Matrix& Current() const { return stack[depth]; }
    void Push() {
        if (depth+1 == stack.size()) throw std::runtime_error("Mot1 matrix stack overflow");
        stack[depth+1] = stack[depth]; ++depth;
    }
    void Pop() {
        if (!depth) throw std::runtime_error("Mot1 matrix stack underflow");
        --depth;
    }
    void Multiply(const Matrix& right) { //00489ba0: four ordered mul.s/add.s, starting at +0
        Matrix result{};
        for (size_t column = 0; column < 4; ++column)
            for (size_t row = 0; row < 4; ++row)
                for (size_t k = 0; k < 4; ++k)
                    result[column*4+row] = ee::FpuAdd(result[column*4+row],
                        ee::FpuMul(stack[depth][k*4+row],right[column*4+k],nullptr),nullptr);
        stack[depth] = result;
    }
    void Translate(const uint32_t xyz[3]) {
        Matrix m{}; for (size_t i = 0; i < 4; ++i) m[5*i] = 0x3f800000;
        for (size_t i = 0; i < 3; ++i) m[12+i] = xyz[i];
        Multiply(m);
    }
    void Scale(const uint32_t xyz[3]) {
        Matrix m{}; m[15] = 0x3f800000;
        //0042cc78 writes ONE to all three translation lanes, not ZERO.
        m[12] = m[13] = m[14] = 0x3f800000;
        for (size_t i = 0; i < 3; ++i) m[5*i] = xyz[i];
        Multiply(m);
    }
    void Quaternion(const uint32_t quat[4]) {
        Matrix m{}; const uint32_t zero[3]{}; QuatToMatrix(m.data(),zero,quat); Multiply(m);
    }
    void Rotate(size_t axis,uint32_t radians) {
        if (axis >= 3) throw std::runtime_error("Mot1 invalid rotation axis");
        uint32_t quat[4]{};
        LibmSinCos(ee::FpuMul(radians,0x3f000000,nullptr),quat+axis,quat+3);
        Quaternion(quat);
    }
};
// Render callback0042c818..0042c928. Only scene matrix state is exposed;
// unrelated VU scratch registers and stack-memory layout are not API outputs.
struct MotionRenderTransform {
    Vu0Regs vu;
    std::array<uint8_t,16384> scratch{};
    std::vector<MotionTransform::Matrix> stack;
    MotionRenderTransform() { vu.vf[0].f[3] = 0x3f800000; Vu0MatIdentity(vu,scratch.data()); }
    MotionTransform::Matrix Current() const {
        MotionTransform::Matrix result; Vu0MatStore(vu,result.data()); return result;
    }
    void Push() { stack.push_back(Current()); }
    void Pop() {
        if (stack.empty()) throw std::runtime_error("Mot1 render matrix stack underflow");
        Vu0MatLoad(vu,scratch.data(),stack.back().data()); stack.pop_back();
    }
    void Translate(const uint32_t xyz[3]) { Vu0MatTranslate(vu,scratch.data(),xyz[0],xyz[1],xyz[2]); }
    void Scale(const uint32_t xyz[3]) { Vu0MatScale(vu,scratch.data(),xyz[0],xyz[1],xyz[2]); }
    void Quaternion(const uint32_t quat[4]) {
        MotionTransform::Matrix matrix; const uint32_t zero[3]{};
        QuatToMatrix(matrix.data(),zero,quat); Vu0MatMultiply(vu,scratch.data(),matrix.data());
    }
    void Rotate(size_t axis,uint32_t radians) {
        const uint32_t degrees = ee::FpuMul(radians,0x42652ee0,nullptr);
        if (axis == 0) Vu0MatRotateX(vu,scratch.data(),degrees);
        else if (axis == 1) Vu0MatRotateY(vu,scratch.data(),degrees);
        else if (axis == 2) Vu0MatRotateZ(vu,scratch.data(),degrees);
        else throw std::runtime_error("Mot1 invalid render rotation axis");
    }
};
struct MotionPalette {
    std::vector<MotionTransform::Matrix> matrices;
    std::vector<bool> assigned;
    bool hasRoot = false;
    std::array<uint32_t,3> root{};
    explicit MotionPalette(size_t count) : matrices(count), assigned(count,false) {}
    //0042a660 bind branch: rest matrix times current, capture first root translation,
    //subtract that root with EE arithmetic, restore the unchanged current matrix.
    void Bind(MotionRenderTransform& transform,const uint32_t rest[16],uint32_t index) {
        if (index >= matrices.size()) throw std::runtime_error("Mot1 palette index out of range");
        transform.Push(); Vu0MatMultiply(transform.vu,transform.scratch.data(),rest);
        auto matrix = transform.Current();
        if (!hasRoot) { for (size_t i = 0; i < 3; ++i) root[i] = matrix[12+i]; hasRoot = true; }
        for (size_t i = 0; i < 3; ++i) matrix[12+i] = ee::FpuSub(matrix[12+i],root[i],nullptr);
        matrices[index] = matrix; assigned[index] = true; transform.Pop();
    }
};
} // namespace gt4::game
