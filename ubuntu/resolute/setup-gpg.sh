#!/bin/sh

KEYID=4A65FFE4EEFE2E93

ln -s $PWD/gnupg ~/.gnupg

if ! [ -f ~/.gnupg/gpg-agent.conf ] ; then
    cat > ~/.gnupg/gpg-agent.conf <<'EOF'
pinentry-program /usr/bin/pinentry-curses
default-cache-ttl 3600
max-cache-ttl 3600
EOF

    chmod 600 ~/.gnupg/gpg-agent.conf
fi

export GPG_TTY=$(tty)
gpg-connect-agent updatestartuptty /bye

echo test | gpg \
    --local-user $KEYID \
    --clearsign >/dev/null

echo test2 | gpg \
    --local-user $KEYID \
    --clearsign >/dev/null
