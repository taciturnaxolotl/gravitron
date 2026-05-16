{
  description = "Robot Board — ESP32 + Arduino firmware";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
    esp-dev.url = "github:mirrexagon/nixpkgs-esp-dev";
  };

  outputs = { self, nixpkgs, esp-dev }:
    let
      system = "aarch64-darwin";
      pkgs = import nixpkgs { inherit system; };
    in {
      # Default shell for Arduino — use .#esp32 for ESP-IDF work
      devShells.${system}.default = pkgs.mkShell {
        name = "robot-board";
        buildInputs = with pkgs; [ arduino-cli ];
        shellHook = ''
          echo "Robot Board — Arduino ready (use 'nix develop .#esp32' for ESP32)";
        '';
      };
    };
}
