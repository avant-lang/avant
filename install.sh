#!/usr/bin/env bash
set -euo pipefail

# Avant install (D147). Linux x86_64 with glibc: download the release
# archive from GitHub, check its SHA-256, install it to ~/.avant, check the
# glibc floor (D152), and compile and run a hello program as a self-test.
#
#   curl -fsSL https://github.com/avant-lang/avant/releases/latest/download/install.sh | bash
#   bash install.sh [VERSION] [--no-modify-path]
#
# VERSION (or AVANT_VERSION) picks a release, for example v0.34.0; the
# default is the latest. AVANT_INSTALL moves the install directory.
# --no-modify-path leaves shell configuration alone and prints the PATH line.
# Adapted from the Alumna installer (https://alumna.dev/install).

Color_Off=''
Red=''
Green=''
Yellow=''
Dim=''
Bold_White=''
Bold_Green=''

if [[ -t 1 ]]; then
	Color_Off='\033[0m'
	Red='\033[0;31m'
	Green='\033[0;32m'
	Yellow='\033[0;33m'
	Dim='\033[0;2m'
	Bold_White='\033[1m'
	Bold_Green='\033[1;32m'
fi

error() {
	echo -e "${Red}error${Color_Off}: $*" >&2
	exit 1
}

warn() {
	echo -e "${Yellow}warning${Color_Off}: $*" >&2
}

info() {
	echo -e "${Dim}$*${Color_Off}"
}

info_bold() {
	echo -e "${Bold_White}$*${Color_Off}"
}

success() {
	echo -e "${Green}$*${Color_Off}"
}

version=''
modify_path=1
for arg in "$@"; do
	case $arg in
	--no-modify-path)
		modify_path=0
		;;
	-h | --help)
		sed -n '4,15p' "$0" 2>/dev/null | sed 's/^# \{0,1\}//'
		exit 0
		;;
	-*)
		error "Unknown option \"$arg\". The options are an optional version (for example v0.34.0) and --no-modify-path."
		;;
	*)
		[[ -z $version ]] || error 'Too many arguments. Only an optional version and --no-modify-path are allowed.'
		version=$arg
		;;
	esac
done

platform=${AVANT_INTERNAL_UNAME:-$(uname -ms)}

case $platform in
'Linux x86_64') ;;
*)
	error "Avant has no binary for \"$platform\" yet (only Linux x86_64 with glibc)"
	;;
esac

is_musl() {
	if [[ -n ${AVANT_INTERNAL_MUSL:-} ]]; then
		[[ $AVANT_INTERNAL_MUSL = 1 ]]
		return
	fi
	# System libc, not "is a musl loader present". Debian/Ubuntu can install
	# the musl package next to glibc; those hosts need the glibc binary.
	if command -v getconf >/dev/null 2>&1 && getconf GNU_LIBC_VERSION >/dev/null 2>&1; then
		return 1
	fi
	if [[ -f /etc/alpine-release ]]; then
		return 0
	fi
	if command -v ldd >/dev/null 2>&1; then
		if ldd --version 2>&1 | grep -qi musl; then
			return 0
		fi
		if ldd /bin/sh 2>&1 | grep -qi musl; then
			return 0
		fi
	fi
	return 1
}

if is_musl; then
	error 'Avant has no binary for musl systems yet (only Linux x86_64 with glibc)'
fi

# The machine's glibc version, for example 2.39; empty when unknown.
glibc_version() {
	if [[ -n ${AVANT_INTERNAL_GLIBC:-} ]]; then
		echo "$AVANT_INTERNAL_GLIBC"
		return
	fi
	getconf GNU_LIBC_VERSION 2>/dev/null | awk '{ print $2 }'
}

# True when version $1 is at least version $2 (dotted numbers).
version_at_least() {
	[[ $(printf '%s\n%s\n' "$2" "$1" | sort -V | head -n 1) = "$2" ]]
}

command -v curl >/dev/null || error 'curl is required to install Avant'
command -v tar >/dev/null || error 'tar is required to install Avant'

if [[ -z $version ]]; then
	version=${AVANT_VERSION:-latest}
fi
if [[ $version != latest && $version != v* ]]; then
	version="v$version"
fi

GITHUB=${GITHUB:-https://github.com}
github_repo="$GITHUB/avant-lang/avant"
asset="avant-linux-x86_64.tar.gz"

if [[ -n ${AVANT_INTERNAL_DOWNLOAD_BASE:-} ]]; then
	base_uri=$AVANT_INTERNAL_DOWNLOAD_BASE
elif [[ $version = latest ]]; then
	base_uri="$github_repo/releases/latest/download"
else
	base_uri="$github_repo/releases/download/$version"
fi

archive_uri="$base_uri/$asset"
sum_uri="$base_uri/$asset.sha256"

install_dir=${AVANT_INSTALL:-$HOME/.avant}
install_dir=${install_dir%/}
[[ -n $install_dir && $install_dir != / ]] || error "Refusing to install into \"${install_dir:-/}\""
exe=$install_dir/avant

# The install directory is replaced as a whole, so it must be one this
# script made (or empty, or absent): never a directory with other files.
if [[ -e $install_dir ]]; then
	[[ -d $install_dir ]] || error "\"$install_dir\" exists and is not a directory"
	if [[ -n $(ls -A "$install_dir") && ! -f $install_dir/.avant-install ]]; then
		error "\"$install_dir\" is not empty and is not an Avant install; choose another directory with AVANT_INSTALL"
	fi
fi

parent=$(dirname "$install_dir")
mkdir -p "$parent" || error "Failed to create \"$parent\""
tmp=$(mktemp -d "$parent/.avant-install-XXXXXX") || error "Failed to create a temporary directory in \"$parent\""
trap 'rm -rf "$tmp"' EXIT

info "Downloading $asset ($version)"
curl --fail --location --progress-bar --output "$tmp/$asset" "$archive_uri" ||
	error "Failed to download Avant from \"$archive_uri\""

curl --fail --location --silent --output "$tmp/$asset.sha256" "$sum_uri" ||
	error "Failed to download the checksum from \"$sum_uri\""

want=$(awk 'NR == 1 { print $1 }' "$tmp/$asset.sha256")
[[ $want =~ ^[0-9a-f]{64}$ ]] || error "No checksum in \"$sum_uri\""

if command -v sha256sum >/dev/null; then
	got=$(sha256sum "$tmp/$asset" | awk '{ print $1 }')
elif command -v shasum >/dev/null; then
	got=$(shasum -a 256 "$tmp/$asset" | awk '{ print $1 }')
else
	error 'sha256sum or shasum is required to verify the download'
fi

if [[ $got != "$want" ]]; then
	error "Checksum mismatch for $asset"
fi

mkdir "$tmp/x"
tar -xzf "$tmp/$asset" -C "$tmp/x" || error 'Failed to extract Avant'

new="$tmp/x/avant-linux-x86_64"
[[ -f $new/avant ]] || error 'The archive did not contain avant-linux-x86_64/avant'

# The glibc floor (D152): the release records the oldest glibc its binaries
# and runtime objects work with.
floor=$(awk -F': *' '$1 == "glibc-floor" { print $2 }' "$new/versions.txt" 2>/dev/null || true)
if [[ -n $floor ]]; then
	have=$(glibc_version)
	if [[ -z $have ]]; then
		warn "cannot tell this system's glibc version; Avant $version needs glibc $floor or newer"
	elif ! version_at_least "$have" "$floor"; then
		error "Avant $version needs glibc $floor or newer; this system has glibc $have"
	fi
fi

touch "$new/.avant-install"
if [[ -e $install_dir ]]; then
	mv "$install_dir" "$tmp/old" || error "Failed to move the previous install out of \"$install_dir\""
fi
if ! mv "$new" "$install_dir"; then
	[[ -e $tmp/old ]] && mv "$tmp/old" "$install_dir"
	error "Failed to write \"$install_dir\""
fi

tildify() {
	if [[ $1 = "$HOME"/* ]]; then
		echo "~/${1#"$HOME"/}"
	else
		echo "$1"
	fi
}

success "Avant was installed to ${Bold_Green}$(tildify "$install_dir")${Color_Off}"

# Prerequisites. Programs are linked by the system C compiler, which needs
# the C library's development files; the self-test proves the whole chain.
os_like=''
if [[ -r /etc/os-release ]]; then
	os_like=$( (. /etc/os-release && echo "${ID:-} ${ID_LIKE:-}") || true)
fi

cc_hint() {
	case " $os_like " in
	*" debian "* | *" ubuntu "*) echo 'sudo apt install gcc libc6-dev' ;;
	*" fedora "* | *" rhel "* | *" centos "*) echo 'sudo dnf install gcc glibc-devel' ;;
	*" suse "* | *" opensuse "*) echo 'sudo zypper install gcc glibc-devel' ;;
	*" arch "*) echo 'sudo pacman -S gcc' ;;
	*) echo 'install a C compiler (gcc or clang) and the C library development files' ;;
	esac
}

clang_hint() {
	case " $os_like " in
	*" debian "* | *" ubuntu "*) echo 'sudo apt install libclang1-20' ;;
	*" fedora "* | *" rhel "* | *" centos "*) echo 'sudo dnf install clang-libs' ;;
	*) echo 'install libclang 20' ;;
	esac
}

self_test_ok=0
if ! command -v cc >/dev/null 2>&1; then
	warn "no C compiler (cc) found: Avant links programs with it. To install one: $(cc_hint)"
else
	t="$tmp/self-test"
	mkdir -p "$t"
	printf 'fn main {\n  puts("hello from avant")\n}\n' >"$t/hello.av"
	if out=$(cd "$t" && env -u AVANT_ROOT -u AVANT_MYC_LLVM -u AVANT_RE2C -u AVANT_BACKEND TMPDIR="$t" "$exe" run hello.av </dev/null 2>&1) &&
		[[ $out = "hello from avant" ]]; then
		self_test_ok=1
		info "Self-test: compiled and ran a hello program"
	else
		warn "the self-test could not compile and run a hello program:"
		echo "$out" | head -n 12 | sed 's/^/  /' >&2
		crt=$(cc -print-file-name=crt1.o 2>/dev/null || true)
		if [[ $crt != /* ]]; then
			warn "the C library development files seem to be missing. To install them: $(cc_hint)"
		fi
	fi
fi

if ldd "$install_dir/avant-bind" 2>/dev/null | grep -q 'not found'; then
	info "Optional: 'avant bind' (C header to Avant lib block) needs libclang 20: $(clang_hint)"
fi

if command -v avant >/dev/null && [[ $(command -v avant) = "$exe" ]]; then
	echo "Run 'avant run hello.av' to get started"
	[[ $self_test_ok = 1 ]] || exit 1
	exit 0
fi

quoted_install_dir=${install_dir//\"/\\\"}
path_line="export PATH=\"$quoted_install_dir:\$PATH\""
refresh_command=''

echo

# Appends the PATH lines to a shell config unless an earlier install did.
add_to_config() {
	local config=$1
	shift
	if grep -q '^# avant$' "$config" 2>/dev/null; then
		info "$(tildify "$config") already adds Avant to PATH"
		return
	fi
	{
		echo
		echo '# avant'
		printf '%s\n' "$@"
	} >>"$config"
	info "Added $(tildify "$install_dir") to PATH in $(tildify "$config")"
}

if [[ $modify_path = 0 ]]; then
	echo "Add this directory to PATH:"
	info_bold "  $path_line"
else
	case $(basename "${SHELL:-}") in
	fish)
		fish_config=$HOME/.config/fish/config.fish
		if [[ -w $fish_config ]]; then
			add_to_config "$fish_config" "set --export PATH \"$quoted_install_dir\" \$PATH"
			refresh_command="source $(tildify "$fish_config")"
		else
			echo "Add this directory to PATH:"
			info_bold "  set --export PATH \"$quoted_install_dir\" \$PATH"
		fi
		;;
	zsh)
		zsh_config=$HOME/.zshrc
		if [[ -w $zsh_config ]]; then
			add_to_config "$zsh_config" "$path_line"
			refresh_command="exec $SHELL"
		else
			echo "Add this directory to PATH:"
			info_bold "  $path_line"
		fi
		;;
	bash)
		bash_config=''
		for candidate in "$HOME/.bashrc" "$HOME/.bash_profile"; do
			if [[ -w $candidate ]]; then
				bash_config=$candidate
				break
			fi
		done
		if [[ -n $bash_config ]]; then
			add_to_config "$bash_config" "$path_line"
			refresh_command="source $(tildify "$bash_config")"
		else
			echo "Add this directory to PATH:"
			info_bold "  $path_line"
		fi
		;;
	*)
		echo "Add this directory to PATH:"
		info_bold "  $path_line"
		;;
	esac
fi

echo
info "To get started, run:"
echo
if [[ -n $refresh_command ]]; then
	info_bold "  $refresh_command"
fi
info_bold "  avant run hello.av"
[[ $self_test_ok = 1 ]] || exit 1
