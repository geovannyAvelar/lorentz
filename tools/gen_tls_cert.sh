#!/bin/bash
# This script generates a self-signed certificate valid for 10 years
openssl req -new -newkey rsa:4096 -days 3650 -nodes -x509 \
    -subj "/C=DE/ST=NRW/L=Bonn/O=lorentz.lan/CN=lorentz.lan" \
    -keyout lorentz.key -out lorentz.crt

# Alternatively, generate a ECDSA certificate
# openssl ecparam -out lorentz.key -name prime256v1 -genkey
# openssl req -new -days 3650 -nodes -x509 \
#     -subj "/C=DE/ST=NRW/L=Bonn/O=lorentz.lan/CN=lorentz.lan" \
#     -key lorentz.key -out lorentz.cert

# Combine key and certificate into a single PEM file
cp lorentz.crt lorentz.pem
cat lorentz.key >> lorentz.pem
