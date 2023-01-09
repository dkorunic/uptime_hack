with import <nixpkgs> {};
let
  linux = linux_latest;
in stdenv.mkDerivation {
  name = "env";
  buildInputs = linux.moduleBuildDependencies;
  shellHook = ''
    export KERNELDIR=${linux.dev}/lib/modules/*/build
  '';
  hardeningDisable=["all"];
}
