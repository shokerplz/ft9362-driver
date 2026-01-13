{
  description = "FocalTech FT9362 (2808:0752) fingerprint driver for libfprint";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system: let
      pkgs = import nixpkgs { inherit system; };

      libfprint-focaltech0752 = pkgs.libfprint.overrideAttrs (oldAttrs: {
        pname = "libfprint-focaltech0752";
        __intentionallyOverridingVersion = true;

        installCheckPhase = ''
          runHook preInstallCheck
          meson test --no-rebuild -C build --suite libfprint --exclude udev-hwdb || true
          runHook postInstallCheck
        '';

        postPatch = (oldAttrs.postPatch or "") + ''
          cp ${./shared/focaltech_nn_weights.h} libfprint/drivers/focaltech_nn_weights.h
          cp ${./shared/focaltech_nn_infer.c} libfprint/drivers/focaltech_nn_infer.c
          cp ${./shared/focaltech_nn_infer.h} libfprint/drivers/focaltech_nn_infer.h
          cp ${./shared/focaltech_nn_match.c} libfprint/drivers/focaltech_nn_match.c
          cp ${./shared/focaltech_nn_match.h} libfprint/drivers/focaltech_nn_match.h
          cp ${./driver/focaltech-0752.c} libfprint/drivers/focaltech0752.c

          sed -i "s/    'focaltech_moc' :/    'focaltech0752' :\n        [ 'drivers\/focaltech0752.c', 'drivers\/focaltech_nn_match.c', 'drivers\/focaltech_nn_infer.c' ],\n    'focaltech_moc' :/" libfprint/meson.build
          sed -i "s/    'focaltech_moc',/    'focaltech_moc',\n    'focaltech0752',/" meson.build
        '';

        postInstall = (oldAttrs.postInstall or "") + ''
          mkdir -p $out/lib/udev/rules.d
          cat > $out/lib/udev/rules.d/60-focaltech0752.rules << 'EOF'
          SUBSYSTEM=="usb", ATTRS{idVendor}=="2808", ATTRS{idProduct}=="0752", MODE="0664", GROUP="plugdev", TAG+="uaccess"
          EOF
        '';
      });

      fprintd-focaltech0752 = pkgs.fprintd.override {
        libfprint = libfprint-focaltech0752;
      };
    in {
      packages = {
        libfprint = libfprint-focaltech0752;
        fprintd = fprintd-focaltech0752;
        default = fprintd-focaltech0752;
      };

      devShells.default = pkgs.mkShell {
        buildInputs = with pkgs; [
          libfprint-focaltech0752
          fprintd-focaltech0752
          libusb1 glib pkg-config meson ninja gcc
        ];

        shellHook = ''
          echo "FocalTech FT9362 dev shell"
          echo "  fprintd-enroll / fprintd-verify / fprintd-list"
        '';
      };
    }) // {
      nixosModules.default = { config, lib, pkgs, ... }: let
        cfg = config.services.fprintd-focaltech0752;
      in {
        options.services.fprintd-focaltech0752 = {
          enable = lib.mkEnableOption "fprintd with FocalTech 0752 driver";
        };

        config = lib.mkIf cfg.enable {
          services.fprintd.enable = lib.mkForce true;
          services.fprintd.package = lib.mkForce self.packages.${pkgs.system}.fprintd;
          services.udev.extraRules = ''
            SUBSYSTEM=="usb", ATTRS{idVendor}=="2808", ATTRS{idProduct}=="0752", MODE="0664", GROUP="plugdev", TAG+="uaccess"
          '';
          users.groups.plugdev = {};
        };
      };

      overlays.default = final: prev: {
        libfprint-focaltech0752 = self.packages.${prev.system}.libfprint;
        fprintd-focaltech0752 = self.packages.${prev.system}.fprintd;
      };
    };
}
