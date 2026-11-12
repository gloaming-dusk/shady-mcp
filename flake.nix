{
  description = "shady-mcp: computer use on the Shady compositor, as an MCP server";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";
    # For the tests: a headless Shady to drive.
    shady.url = "github:gloaming-dusk/shady";
    shady.inputs.nixpkgs.follows = "nixpkgs";
  };

  outputs = { self, nixpkgs, shady, ... }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
      packageFor = pkgs: pkgs.stdenv.mkDerivation {
        pname = "shady-mcp";
        version = "0.1.0";
        src = self;
        nativeBuildInputs = with pkgs; [ meson ninja ];
        meta = {
          description = "MCP server for computer use on the Shady Wayland compositor";
          homepage = "https://github.com/gloaming-dusk/shady-mcp";
          license = pkgs.lib.licenses.asl20;
          mainProgram = "shady-mcp";
          platforms = pkgs.lib.platforms.linux;
        };
      };
    in {
      packages = forAllSystems (pkgs: rec {
        shady-mcp = packageFor pkgs;
        default = shady-mcp;
      });

      overlays.default = final: prev: { shady-mcp = packageFor final; };

      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          packages = with pkgs; [ meson ninja gcc python3 foot ]
            ++ [ shady.packages.${pkgs.stdenv.hostPlatform.system}.default ];
        };
      });
    };
}
