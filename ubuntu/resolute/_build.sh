
function build-packages {
    source /etc/lsb-release

    if [[ "$DISTRIB_CODENAME" != resolute ]] ; then
        echo "Expected to be run on ubuntu:26.04" >&2
        exit 1
    fi

    # output directory
    rm -rf packages/$PACKAGE
    mkdir -p packages/$PACKAGE

    # work directory
    mkdir -p $PACKAGE

    ROOT="$PWD"

    pushd $PACKAGE
        BASE="$PWD"

        # install build tools and dependencies
        sudo apt -y build-dep $PACKAGE
        sudo apt -y install $EXTRA_DEPENDENCIES

        # clone repository
        if ! [ -e repo/ok ] ; then
            git clone https://github.com/weaselway/$REPO.git repo
            touch repo/ok
        fi

        # checkout branch for ubuntu -- fetched every time, otherwise a
        # second build would keep packaging whatever the first clone saw
        git -C repo fetch origin
        git -C repo switch -C $BRANCH origin/$BRANCH

        # export get patches
        rm -rf patches
        mkdir patches
        git -C repo format-patch -o $BASE/patches $UPSTREAMTAG..HEAD

        # extract debian package source, pinned to the upstream version the
        # branch is based on: after an archive bump a plain `apt source` would
        # unpack a different version and the patches (and $PACKAGE_SOURCE)
        # would no longer match
        UPSTREAM_VERSION=${PACKAGE_SOURCE#"$PACKAGE"-}
        SOURCE_VERSION=$(apt-cache showsrc $PACKAGE | sed -n 's/^Version: //p' |
            grep "^${UPSTREAM_VERSION}-" | sort -V | tail -n 1 || true)
        if [ -z "$SOURCE_VERSION" ] ; then
            echo "The archive has no $PACKAGE source for $UPSTREAM_VERSION any more;" \
                 "rebase $BRANCH and update PACKAGE_SOURCE" >&2
            exit 1
        fi

        rm -rf $PACKAGE_SOURCE
        apt source $PACKAGE=$SOURCE_VERSION

        pushd $PACKAGE_SOURCE
            if [ -f $ROOT/debian-$PACKAGE.patch ] ; then
                # apply patches to mutter packaging
                patch -p1 < $ROOT/debian-$PACKAGE.patch
            fi

            export QUILT_PATCHES=debian/patches

            # import and apply patches
            quilt import $BASE/patches/*.patch
            quilt push -a

            export DEBFULLNAME="Oliver Bestmann"
            export DEBEMAIL="oliver.bestmann@googlemail.com"

            if $SOURCEONLY ; then
                # build source package only, can be uploaded to launchpad ppa
                echo | dch --local "+${RELEASE_SUFFIX}." "Custom build for weasel"
                dch --release "Weasel build release"

                dpkg-parsechangelog -S Version
                dpkg-buildpackage -S -sa -d -k4A65FFE4EEFE2E93
            else
                # build local binary packages
                echo | dch --local "+${RELEASE_SUFFIX}." "Custom build for weasel"
                dpkg-buildpackage -us -uc -b
            fi
        popd

        # move files to package directory
        shopt -s nullglob
        mv *.deb *.ddeb *.buildinfo *.changes *.tar.xz *.dsc *.asc ../packages/$PACKAGE/
    popd
}
