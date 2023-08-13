{ stdenv
, cmake
, ninja
, pkg-config
, libX11
, libXext
, libXres

, ...
}:

stdenv.mkDerivation {
  name = "bswallow";
  version = "0.1.0";

  src = ./.;
  nativeBuildInputs = [
    cmake
    ninja
    pkg-config
  ];

  buildInputs = [
    libX11
    libXext
    libXres
  ];

  cmakeFlags = [
    ''-DCMAKE_INSTALL_PREFIX=''${out}''
  ];
}
