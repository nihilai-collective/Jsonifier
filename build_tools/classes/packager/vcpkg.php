<?php

namespace Jsonifier\Packager;

use \RuntimeException;

const GREEN = "\033[32m";
const RED   = "\033[31m";
const WHITE = "\033[0m";

class Vcpkg
{
    private string $latestTag;
    private string $version;
    private string $git;
    private string $sudo;
    private bool $firstBuildComplete = false;

    public function __construct()
    {
        global $argv;
        if (count($argv) < 3) {
            throw new RuntimeException(RED . "Missing github repository owner and access token\n" . WHITE);
        }
        echo GREEN . "Starting vcpkg updater...\n" . WHITE;

        $this->latestTag = preg_replace("/\n/", "", shell_exec("git describe --tags `git rev-list --tags --max-count=1`"));
        $this->version   = preg_replace('/^v/', '', $this->getTag());
        echo GREEN . "Latest tag: " . $this->getTag() . " version: " . $this->getVersion() . "\n" . WHITE;

        $this->git  = trim(`which git`);
        $this->sudo = trim(`which sudo`);
    }

    public function getVersion(): string
    {
        return $this->version;
    }

    public function getTag(): string
    {
        return $this->latestTag;
    }

    private function git(string $parameters, bool $sudo = false): void
    {
        system(($sudo ? $this->sudo . ' ' : '') . $this->git . ' ' . $parameters);
    }

    private function sudo(string $command): void
    {
        system($this->sudo . ' ' . $command);
    }

    public function checkoutRepository(string $tag = ""): bool
    {
        global $argv;

        if (empty($tag)) {
            $tag = trim(shell_exec("{$this->git} config --get init.defaultBranch 2>/dev/null || echo main"));
        }

        $repositoryUrl = 'https://' . urlencode($argv[1]) . ':' . urlencode($argv[2]) . '@github.com/nihilai-collective/Jsonifier';

        echo GREEN . "Check out repository: $tag (user: " . $argv[1] . " branch: " . $tag . ")\n" . WHITE;

        chdir(getenv('HOME'));
        system('rm -rf ./jsonifier');
        $this->git('config --global user.email "40668522+RealTimeChris@users.noreply.github.com"');
        $this->git('config --global user.name "RealTimeChris"');
        $this->git('clone ' . escapeshellarg($repositoryUrl) . ' ./jsonifier --depth=1');

        $status = chdir(getenv("HOME") . '/jsonifier');
        $this->git('fetch -at 2>/dev/null');
        $this->git('checkout ' . escapeshellarg($tag) . ' 2>/dev/null');

        return $status;
    }

    public function constructPortAndVersionFile(string $sha512 = "0"): string
    {
        echo GREEN . "Construct portfile for " . $this->getVersion() . ", sha512: $sha512\n" . WHITE;
        chdir(getenv("HOME") . '/jsonifier');

        $portFileContent = 'vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO nihilai-collective/jsonifier
    REF "v${VERSION}"
    SHA512 ' . $sha512 . '
    HEAD_REF main
)

set(VCPKG_BUILD_TYPE release)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
)

vcpkg_cmake_install()

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/License.md" "${SOURCE_PATH}/Third_Party_Licenses.md")
';

        $versionFileContent = '{
  "name": "jsonifier",
  "version": ' . json_encode($this->getVersion()) . ',
  "description": "A few classes for parsing and serializing json - very rapidly.",
  "homepage": "https://github.com/nihilai-collective/jsonifier",
  "license": "MIT",
  "supports": "(windows & x64 & !xbox) | (linux & x64) | (osx & x64)",
  "dependencies": [
    {
      "name": "vcpkg-cmake",
      "host": true
    }
  ]
}';

        echo GREEN . "Writing portfile...\n" . WHITE;
        file_put_contents('./vcpkg/ports/jsonifier/vcpkg.json', $versionFileContent);
        return $portFileContent;
    }

    public function updateVersionFile(): void
    {
        echo GREEN . "Updating VERSION file to " . $this->getVersion() . "\n" . WHITE;
        chdir(getenv("HOME") . '/jsonifier');
        file_put_contents('./VERSION', $this->getVersion() . "\n");
    }

    public function firstBuild(string $portFileContent): string
    {
        echo GREEN . "Starting first build\n" . WHITE;

        chdir(getenv("HOME") . '/jsonifier');
        echo GREEN . "Create /usr/local/share/vcpkg/ports/jsonifier/\n" . WHITE;
        $this->sudo('mkdir -p /usr/local/share/vcpkg/ports/jsonifier/');
        echo GREEN . "Copy vcpkg.json to /usr/local/share/vcpkg/ports/jsonifier/vcpkg.json\n" . WHITE;
        $this->sudo('cp -v -R ./vcpkg/ports/jsonifier/vcpkg.json /usr/local/share/vcpkg/ports/jsonifier/vcpkg.json');

        file_put_contents('/tmp/portfile', $portFileContent);
        $this->sudo('cp -v -R /tmp/portfile /usr/local/share/vcpkg/ports/jsonifier/portfile.cmake');
        unlink('/tmp/portfile');

        $buildResults = shell_exec($this->sudo . ' /usr/local/share/vcpkg/vcpkg install jsonifier:x64-linux 2>&1');

        file_put_contents('/tmp/vcpkg_first_build.log', $buildResults ?? '');

        $matches = [];
        if (preg_match('/please change the expected SHA512 to:\s+([0-9a-fA-F]+)/s', $buildResults ?? '', $matches)) {
            echo GREEN . "Obtained SHA512 (canonical msg): " . $matches[1] . "\n" . WHITE;
            $this->firstBuildComplete = true;
            return $matches[1];
        }

        if (preg_match('/Actual\s*:\s*([0-9a-fA-F]{128})/s', $buildResults ?? '', $matches)) {
            echo GREEN . "Obtained SHA512 (unexpected-hash msg): " . $matches[1] . "\n" . WHITE;
            $this->firstBuildComplete = true;
            return $matches[1];
        }

        echo RED . "No SHA512 found during first build :(\n" . WHITE;

        echo RED . "No SHA512 found during first build :(\n" . WHITE;
        echo RED . "Full vcpkg output saved to /tmp/vcpkg_first_build.log\n" . WHITE;
        echo RED . "Raw output:\n" . ($buildResults ?? '(empty)') . "\n" . WHITE;
        return '';
    }

   public function secondBuild(string $portFileContent): bool
     {
         if (!$this->firstBuildComplete) {
             throw new RuntimeException("No SHA512 sum is available, first build has not been run!");
         }
         $portFileContent = str_replace("\r\n", "\n", $portFileContent);
         echo GREEN . "Executing second build\n" . WHITE;
         echo GREEN . "Copy local port files to /usr/local/share...\n" . WHITE;
         $this->git('config --global core.autocrlf false', true);
         $this->git('config --global core.eol lf', true);
         $this->git('config core.autocrlf false', true);
         $this->git('config core.eol lf', true);
         chdir(getenv("HOME") . '/jsonifier');
         file_put_contents('./vcpkg/ports/jsonifier/portfile.cmake', $portFileContent);
         $this->sudo('cp -v -R ./vcpkg/ports/jsonifier/vcpkg.json /usr/local/share/vcpkg/ports/jsonifier/vcpkg.json');
         $this->sudo('cp -v -R ./vcpkg/ports/jsonifier/portfile.cmake /usr/local/share/vcpkg/ports/jsonifier/portfile.cmake');
         $this->sudo('cp -v -R ./vcpkg/ports/* /usr/local/share/vcpkg/ports/');

         echo GREEN . "vcpkg x-add-version...\n" . WHITE;
         chdir('/usr/local/share/vcpkg');
         $this->sudo('./vcpkg format-manifest ./ports/jsonifier/vcpkg.json');
     
         $this->normalizeDirectoryLineEndings('./ports/jsonifier');

         $this->git('add .', true);
         $this->git('commit -m "VCPKG info update"', true);
         $this->sudo('/usr/local/share/vcpkg/vcpkg x-add-version --overwrite-version --all');

         echo GREEN . "Copy back port files from /usr/local/share...\n" . WHITE;
         chdir(getenv('HOME') . '/jsonifier');
         system('cp -v -R /usr/local/share/vcpkg/ports/jsonifier/vcpkg.json ./vcpkg/ports/jsonifier/vcpkg.json');
         system('cp -v -R /usr/local/share/vcpkg/versions/j-/jsonifier.json ./vcpkg/versions/j-/jsonifier.json');

         echo GREEN . "Commit and push changes to main branch\n" . WHITE;
         $this->git('checkout -B main');

         $this->updateVersionFile();

         $this->normalizeDirectoryLineEndings('./vcpkg');

         $this->git('add .');
         $this->git('commit -m "VCPKG info update [skip ci]"');
         $this->git('push origin HEAD:main');

         echo GREEN . "vcpkg install...\n" . WHITE;
         $resultCode = 0;
         $output     = [];
         exec($this->sudo . ' /usr/local/share/vcpkg/vcpkg install jsonifier:x64-linux 2>&1', $output, $resultCode);

         if ($resultCode != 0) {
             echo RED . "There were build errors!\n\nBuild log:\n" . WHITE;
             echo implode("\n", $output) . "\n";
             $logPath = "/usr/local/share/vcpkg/buildtrees/jsonifier/install-x64-linux-dbg-out.log";
             if (file_exists($logPath)) {
                 readfile($logPath);
             }
         }

         return $resultCode == 0;
     }

     private function normalizeDirectoryLineEndings(string $dirPath): void
     {
         if (!is_dir($dirPath)) {
             return;
         }

         $iterator = new \RecursiveIteratorIterator(new \RecursiveDirectoryIterator($dirPath));
         foreach ($iterator as $file) {
             if ($file->isFile()) {
                 $path = $file->getRealPath();
                 $content = file_get_contents($path);
                 if (str_contains($content, "\r\n")) {
                     $normalized = str_replace("\r\n", "\n", $content);
                     file_put_contents($path, $normalized, LOCK_EX);
                 }
             }
         }
     }
}

$vcpkg       = new Vcpkg();
$checkedOut  = $vcpkg->checkoutRepository($vcpkg->getTag());

if (!$checkedOut) {
    echo RED . "Failed to checkout repository!\n" . WHITE;
    exit(1);
}

$portFile = $vcpkg->constructPortAndVersionFile();
$sha512   = $vcpkg->firstBuild($portFile);

if (empty($sha512)) {
    echo RED . "Aborting: could not obtain SHA512 from first build.\n" . WHITE;
    exit(1);
}

$portFile = $vcpkg->constructPortAndVersionFile($sha512);
$success  = $vcpkg->secondBuild($portFile);

exit($success ? 0 : 1);