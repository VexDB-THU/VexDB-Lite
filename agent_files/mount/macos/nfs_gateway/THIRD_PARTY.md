# Third-party dependency

The macOS NFS gateway vendors `nfsserve` 0.11.0 from
<https://github.com/huggingface/nfsserve> under its BSD-3-Clause license.
The local fork adds the NFSv3 LINK and COMMIT procedures required by the
VexFS mount contract. Its license is kept at `vendor/nfsserve/LICENSE`.

Cargo.lock records the exact transitive dependency versions used by the build.
