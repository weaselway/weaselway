FROM ubuntu:26.04

# enable deb-src
RUN sed -i 's/^Types: deb$/Types: deb deb-src/' \
    /etc/apt/sources.list.d/ubuntu.sources

# for mesa & mutter
RUN apt -y update \
 && apt -y install sudo dpkg-dev build-essential devscripts quilt git

# for kernel module
RUN apt -y update \
  && apt -y install \
         git gcc make flex bison bc pahole rsync diffutils \
         openssl libssl-dev libelf-dev \
         sudo kmod passwd tar xz-utils \
         curl ca-certificates

ARG UID=1000
ARG GID=1000
ARG USERNAME=builder

RUN groupadd -o -g "${GID}" "${USERNAME}" \
    && useradd -o -u "${UID}" -g "${GID}" -m -s /bin/bash "${USERNAME}" \
    && printf '#%s ALL=(ALL) NOPASSWD: ALL\n' "${UID}" >> "/etc/sudoers"

RUN mkdir -p /work && chown "${UID}:${GID}" /work

USER ${UID}:${GID}
WORKDIR /work

ENTRYPOINT [ "/usr/bin/env", "bash" ]
