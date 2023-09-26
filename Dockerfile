FROM alpine

RUN apk add \
      boost-dev                 \
      cmake                     \
      double-conversion-dev     \
      fontconfig-dev            \
      g++                       \
      gc-dev                    \
      gcc                       \
      glib-dev                  \
      glibmm-dev                \
      gsl-dev                   \
      gspell-dev                \
      gtk+3.0-dev               \
      gtkmm3-dev                \
      gtksourceview-dev         \
      gtksourceview4-dev        \
      harfbuzz-dev              \
      lcms2-dev                 \
      libsoup-dev               \
      libxslt-dev               \
      make                      \
      pango-dev                 \
      pkgconf                   \
      poppler-dev               \
      potrace-dev               \
      slirp4netns               \
      vtk                       \
      xfce4                     \
      xvfb                      \
      ; 

ADD . /root/inkscape
WORKDIR /root/inkscape

RUN mkdir build     && \
      cd build      && \
      cmake ..      && \
      make install

ENV DISPLAY=:2

VOLUME /root/output
WORKDIR /root/output

CMD Xvfb :2 -nolisten tcp -shmem & \
  inkscape --actions="select-all;selection-trace:128,false,true,true,0,1.34,3.497;export-filename:/root/output/output.svg;export-do;" /root/inkscape/share/tutorials/tux.png
