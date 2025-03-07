/*
 * Copyright 2022 The TensorFlow Runtime Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef TFRT_BACKENDS_JITRT_JITRT_H_
#define TFRT_BACKENDS_JITRT_JITRT_H_

#include "llvm/Support/Error.h"
#include "third_party/tensorflow/compiler/xla/runtime/arguments.h"

namespace tfrt {

class Tensor;

namespace jitrt {

// Converts tfrt Tensor to the Memref descriptor if concrete Tensor type is
// supported (currently only DenseHostTensor can be converted). Returns error
// otherwise.
llvm::Expected<xla::runtime::MemrefDesc> ConvertTensorToMemrefDesc(
    const Tensor& tensor);

}  // namespace jitrt
}  // namespace tfrt

#endif  // TFRT_BACKENDS_JITRT_JITRT_H_
