# Third-party notices

The optional `bruneton_reference_compare` target downloads and builds the CPU reference implementation from Eric Bruneton's precomputed atmospheric scattering project.

Pinned source revisions:

- `precomputed_atmospheric_scattering`: `34f14e745cff948f4ca3157d1b62a445ffa7286f`
- `dimensional_types`: `84e1e53320cd89119d31ec87cfdc2b51825b0c4b`
- `progress_bar`: `b34cdce09e505e36df90bd7e3b621949f5a52c24`

The source projects are available at:

- https://github.com/ebruneton/precomputed_atmospheric_scattering
- https://github.com/ebruneton/dimensional_types
- https://github.com/ebruneton/progress_bar

The reference implementation and its two CPU-only dependencies are distributed under the BSD 3-Clause License. Their copyright notices are retained in the upstream sources fetched by CMake.

Copyright (c) 2016, 2017 Eric Bruneton
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation
and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors
may be used to endorse or promote products derived from this software without
specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

The dimensional_types dependency has a 2016 copyright notice. The progress_bar dependency has a 2017 copyright notice. Both use the same BSD 3-Clause license terms. Their exact upstream notices are retained in the fetched source trees.
