
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

        # checkout branch for ubuntu
        git -C repo switch $BRANCH

        # export get patches
        rm -rf patches
        mkdir patches
        git -C repo format-patch -o $BASE/patches $UPSTREAMTAG..HEAD

        # extract debian package source
        rm -rf $PACKAGE_SOURCE
        apt source $PACKAGE

        pushd $PACKAGE_SOURCE
            if [ -f $ROOT/debian-$PACKAGE.patch ] ; then
                # apply patches to mutter packaging
                patch -p1 < $ROOT/debian-$PACKAGE.patch
            fi

            export QUILT_PATCHES=debian/patches

            # import and apply patches
            quilt import $BASE/patches/*.patch
            quilt push -a

            if $SOURCEONLY ; then
                # build source package only, can be uploaded to launchpad ppa
                echo | dch --distribution resolute --release
                dpkg-buildpackage -S -sa -d -k4A65FFE4EEFE2E93
            else
                # build local binary packages
                echo | dch --local "+weasel0" "Custom build for weasel"
                dpkg-buildpackage -us -uc -b
            fi
        popd

        # move files to package directory
        shopt -s nullglob
        mv *.deb *.ddeb *.buildinfo *.changes *.tar.xz *.dsc *.asc ../packages/$PACKAGE/
    popd
}
