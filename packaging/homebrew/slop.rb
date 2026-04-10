class Slop < Formula
  desc "Code quality scanner — catches patterns linters miss"
  homepage "https://github.com/en9inerd/slop"
  license "MIT"
  version "VERSION_PLACEHOLDER"

  on_macos do
    if Hardware::CPU.arm?
      url "https://github.com/en9inerd/slop/releases/download/vVERSION_PLACEHOLDER/slop_aarch64-macos"
      sha256 "SHA256_MACOS_ARM64"
    else
      url "https://github.com/en9inerd/slop/releases/download/vVERSION_PLACEHOLDER/slop_x86_64-macos"
      sha256 "SHA256_MACOS_X86_64"
    end
  end

  on_linux do
    if Hardware::CPU.arm?
      url "https://github.com/en9inerd/slop/releases/download/vVERSION_PLACEHOLDER/slop_aarch64-linux-musl"
      sha256 "SHA256_LINUX_ARM64"
    else
      url "https://github.com/en9inerd/slop/releases/download/vVERSION_PLACEHOLDER/slop_x86_64-linux-musl"
      sha256 "SHA256_LINUX_X86_64"
    end
  end

  def install
    binary = Dir["slop*"].first
    bin.install binary => "slop"
  end

  test do
    assert_match "slop", shell_output("#{bin}/slop --help")
  end
end
