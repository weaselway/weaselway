{
  description = "weaselway install/packaging scripts dev environment";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";
  };

  outputs =
    { self, nixpkgs }:
    let
      forAllSystems =
        f:
        nixpkgs.lib.genAttrs [ "x86_64-linux" "aarch64-linux" ] (
          system: f nixpkgs.legacyPackages.${system}
        );
    in
    {
      # Nothing here compiles; the scripts run inside the target distro. The
      # closest thing to a build is linting them.
      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          packages = with pkgs; [
            bash
            shellcheck
            git
          ];

          shellHook = ''
            echo "lint with: shellcheck --shell=bash \$(git ls-files '*.sh')"
          '';
        };
      });

      checks = forAllSystems (pkgs: {
        shellcheck = pkgs.runCommand "weaselway-shellcheck" { nativeBuildInputs = [ pkgs.shellcheck ]; } ''
          cd ${self}
          find . -name '*.sh' -print0 | xargs -0 shellcheck --shell=bash --severity=error
          touch $out
        '';
      });
    };
}
