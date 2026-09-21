#!/bin/python3
# Lorentz: A black hole for Internet advertisements
# (c) 2023 Pi-hole, LLC (https://pi-hole.net)
# Network-wide ad blocking via your own hardware.
#
# Lorentz Engine - auxiliary files
# API test script
#
# This file is copyright under the latest version of the EUPL.
# Please see LICENSE file for your rights under this license.

import io
import ipaddress
import json
import random
import zipfile
from libs.openAPI import openApi
import urllib.request, urllib.parse
from libs.LORENTZAPI import LORENTZAPI, AuthenticationMethods
from collections.abc import MutableMapping

class ResponseVerifyer():

	# Translate between OpenAPI and Python types
	YAML_TYPES = { "string": [str], "integer": [int], "number": [int, float], "boolean": [bool], "array": [list] }
	TELEPORTER_FILES_EXPORT = ["etc/lorentz/gravity.db", "etc/lorentz/lorentz.toml", "etc/lorentz/lorentz.db", "etc/hosts"]
	TELEPORTER_FILES_IMPORT = ['etc/lorentz/lorentz.toml', 'etc/lorentz/dhcp.leases', 'etc/lorentz/gravity.db->group', 'etc/lorentz/gravity.db->adlist', 'etc/lorentz/gravity.db->adlist_by_group', 'etc/lorentz/gravity.db->domainlist', 'etc/lorentz/gravity.db->domainlist_by_group', 'etc/lorentz/gravity.db->client', 'etc/lorentz/gravity.db->client_by_group' ]

	auth_method = "?"
	teleporter_archive = None

	def __init__(self, lorentz: LORENTZAPI, openapi: openApi):
		self.lorentz = lorentz
		self.openapi = openapi
		self.errors = []


	def __enter__(self):
		return self


	def __exit__(self, exc_type, exc_value, traceback):
		return


	def flatten_dict(self, d: MutableMapping, parent_key: str = '', sep: str ='.') -> MutableMapping:
		items = []
		# Iterate over all items in the dictionary
		for k, v in d.items():
			# Create a new key by appending the current key to the parent key
			new_key = parent_key + sep + k if parent_key else k
			# If the value is a dictionary, recursively flatten it, otherwise
			# simply add it to the list of items
			if isinstance(v, MutableMapping):
				items.extend(self.flatten_dict(v, new_key, sep=sep).items())
			else:
				items.append((new_key, v))
		return dict(items)


	def verify_endpoint(self, endpoint: str):
		# If the endpoint starts with /api, remove this part (it is not
		# part of the YAML specs)
		if endpoint.startswith("/api"):
			endpoint = endpoint[4:]

		method = 'get'
		rcode = '200'
		# Check if the endpoint is defined in the API specs
		if endpoint not in self.openapi.paths:
			self.errors.append("Endpoint " + endpoint + " not found in the API specs")
			return self.errors
		# Check if this endpoint + method are defined in the API specs
		if method not in self.openapi.paths[endpoint]:
			self.errors.append("Method " + method + " not found in the API specs")
			return self.errors

		# Get YAML response schema and examples (if applicable)
		expected_mimetype = None
		# Assign random authentication method so we can test them all
		authentication_method = random.choice([a for a in AuthenticationMethods])
		# Check if the expected response is defined in the API specs
		response_rcode = self.openapi.paths[endpoint][method]['responses'][str(rcode)]
		if 'content' in response_rcode:
			content = response_rcode['content']
			if 'application/json' in content:
				expected_mimetype = 'application/json'
				jsonData = content[expected_mimetype]
				YAMLresponseSchema = jsonData['schema']
				YAMLresponseExamples = jsonData['examples'] if 'examples' in jsonData else None
			elif 'application/zip' in content:
				expected_mimetype = 'application/zip'
				jsonData = content[expected_mimetype]
				# The endpoint requires HEADER authentication
				authentication_method = AuthenticationMethods.HEADER
				YAMLresponseSchema = None
				YAMLresponseExamples = None
			elif 'text/html' in content:
				expected_mimetype = 'text/html'
				jsonData = content[expected_mimetype]
				YAMLresponseSchema = None
				YAMLresponseExamples = None
		else:
			# No response defined
			return self.errors

		# Prepare required parameters (if any)
		Lorentzparameters = []
		if 'parameters' in self.openapi.paths[endpoint][method]:
			YAMLparameters = self.openapi.paths[endpoint][method]['parameters']
			for param in YAMLparameters:
				# We are only handling QUERY parameters here as we're doing GET
				if param['in'] != 'query':
					continue
				# We are only adding required parameters here
				if param['required'] == False:
					continue
				Lorentzparameters.append(param['name'] + "=" + urllib.parse.quote_plus(str(param['example'])))

		# Get Lorentz response
		Lorentzresponse = self.lorentz.GET("/api" + endpoint, Lorentzparameters, expected_mimetype, authentication_method)
		self.auth_method = self.lorentz.auth_method
		if Lorentzresponse is None:
			return self.lorentz.errors

		self.YAMLresponse = {}
		# Checking depends on the expected mimetype
		if expected_mimetype == "application/json":
			additionalProperties = []
			# Check if the response is an object. If so, we have to check it
			# recursively
			if 'type' in YAMLresponseSchema and YAMLresponseSchema['type'] == 'object':
				required = YAMLresponseSchema.get('required', [])
				# Loop over all properties of the object
				for prop in YAMLresponseSchema['properties']:
					self.verify_property(YAMLresponseSchema['properties'], YAMLresponseExamples, Lorentzresponse, [prop], required)

			# Check if the response is a gather-all object. If so, we have
			# to check all objects in the array individually
			elif 'allOf' in YAMLresponseSchema and len(YAMLresponseSchema['allOf']) > 0:
				for i in range(len(YAMLresponseSchema['allOf'])):
					required = YAMLresponseSchema['allOf'][i].get('required', [])
					for prop in YAMLresponseSchema['allOf'][i]['properties']:
						self.verify_property(YAMLresponseSchema['allOf'][i]['properties'], YAMLresponseExamples, Lorentzresponse, [prop], required)
						if 'additionalProperties' in YAMLresponseSchema['allOf'][i]['properties'][prop]:
							additionalProperties.append(prop)

			# If neither of the above is true, the definition is invalid
			else:
				self.errors.append("Top-level response should be either an object or a non-empty allOf/anyOf/oneOf")

			# Finally, we check if there are extra properties in the Lorentz response
			# that are not defined in the API specs

			# Flatten the Lorentz response
			Lorentzflat = self.flatten_dict(Lorentzresponse)
			YAMLflat = self.YAMLresponse

			# Check for properties in Lorentz that are not in the API specs
			for property in Lorentzflat.keys():
				if property not in YAMLflat.keys():
					root_prop = property.split(".")[0]
					# If this is an additional property, we
					# can ignore it as [any-key] is expected
					# to be returned - do not report as
					# something missing
					if root_prop in additionalProperties:
						continue
					self.errors.append("Property '" + property + "' missing in the API specs (1)")

		elif expected_mimetype == "application/zip":
			file_like_object = io.BytesIO(Lorentzresponse)
			with zipfile.ZipFile(file_like_object) as zipfile_obj:
				# Read all the files in the archive and check their CRC's and
				# file headers. Returns the name of the first bad file, or else
				# returns None.
				bad_filename = zipfile_obj.testzip()
				if bad_filename is not None:
					self.errors.append("File " + bad_filename + " in received archive is corrupt.")
				# Try to read lorentz.toml and see if it starts with the expected
				# header block
				try:
					# Check if all expected files are present
					for expected_file in self.TELEPORTER_FILES_EXPORT:
						if expected_file not in zipfile_obj.namelist():
							self.errors.append("File " + expected_file + " is missing in received archive.")
					lorentz_toml = zipfile_obj.read("etc/lorentz/lorentz.toml")
					if not lorentz_toml.startswith(b"# Lorentz configuration file (v"):
						self.errors.append("Received ZIP file's lorentz.toml starts with wrong header")
				except Exception as err:
					self.errors.append("Error during ZIP analysis: " + str(err))

				# Store Teleporter archive for later use
				self.teleporter_archive = Lorentzresponse
		elif expected_mimetype == "text/html":
			# Decode the response if it is bytes
			if type(Lorentzresponse) is bytes:
				Lorentzresponse = Lorentzresponse.decode("utf-8")
			elif type(Lorentzresponse) is not str:
				self.errors.append("Lorentz's response is neither bytes nor string")
			# Check if the document starts with either "<!DOCTYPE html>" or
			# "<html>" (case-insensitive)
			r = Lorentzresponse.lower()
			if not r.startswith("<!doctype html>") and not r.startswith("<html>"):
				self.errors.append("Lorentz's response does not start with <!DOCTYPE html> or <html>")
		else:
			self.errors.append("Checker script does not know how to check for mimetype \"" + expected_mimetype + "\"")

		# Return all errors
		return self.errors


	def verify_teleporter_zip(self, teleporter_archive: bytes):
		# Send the zip file to the Lorentz API
		if teleporter_archive is None:
			self.errors.append("No Teleporter archive available for verification")
			return self.errors

		# Send the archive to the Lorentz API
		Lorentzresponse = self.lorentz.POST("/api/teleporter", None, AuthenticationMethods.HEADER, {"file": ('teleporter.zip', teleporter_archive, 'application/zip')})

		#Compare the response with the expected response
		if Lorentzresponse is None:
			self.errors.append("No response from Lorentz API")
			return self.errors
		if 'files' not in Lorentzresponse:
			self.errors.append("Missing 'files' key in Lorentz response")
			return self.errors
		# Compare Lorentzresponse['files'] with self.TELEPORTER_FILES_IMPORT
		for expected_file in self.TELEPORTER_FILES_IMPORT:
			if expected_file not in Lorentzresponse['files']:
				self.errors.append("File " + expected_file + " is missing in Lorentz response")
				self.errors.append(json.dumps(Lorentzresponse['files'], indent=4))

		return self.errors


	# Check if a string is a valid IPv4 address
	def valid_ipv4(self, addr: str) -> bool:
		# Empty string is valid (0.0.0.0)
		if len(addr) == 0:
			return True
		try:
			if type(ipaddress.ip_address(addr)) is ipaddress.IPv4Address:
				return True
		except ValueError:
			pass
		return False

	# Check if a string is a valid IPv6 address
	def valid_ipv6(self, addr: str) -> bool:
		# Empty string is valid (::)
		if len(addr) == 0:
			return True
		try:
			if type(ipaddress.ip_address(addr)) is ipaddress.IPv6Address:
				return True
		except ValueError:
			pass
		return False


	# Verify a single property's type
	def verify_type(self, prop: any, yaml_type: str, yaml_nullable: bool, yaml_format: str = None):
		# Get the type of the property
		prop_type = type(prop)
		# None is an acceptable reply when this is specified in the API specs
		if prop_type is type(None) and yaml_nullable:
			return True
		# Check if the type is correct using the YAML_TYPES translation table
		if yaml_type not in self.YAML_TYPES:
			self.errors.append("Property type \"" + yaml_type + "\" is not valid in OpenAPI specs")
			return False
		if yaml_format is not None:
			# Check if the format is correct
			if yaml_format == "ipv4" and not self.valid_ipv4(prop):
				self.errors.append("Property \"" + str(prop) + "\" is not a valid IPv4 address")
				return False
			elif yaml_format == "ipv6" and not self.valid_ipv6(prop):
				self.errors.append("Property \"" + str(prop) + "\" is not a valid IPv6 address")
				return False
		return prop_type in self.YAML_TYPES[yaml_type]


	# Verify a single property
	def verify_property(self, YAMLprops: dict, YAMLexamples: dict, Lorentzprops: dict, props: list, required: list = None):
		all_okay = True

		# Build flat path of this property
		flat_path = ".".join([str(p) for p in props])

		# Check if the property is defined in the API specs (unless we know there are "any-key" items here)
		if props[-1] not in YAMLprops:
			self.errors.append("Property '" + flat_path + "' missing in the API specs (2)")
			return False
		YAMLprop = YAMLprops[props[-1]]

		# Check if Lorentz returned null when an object was expected
		if Lorentzprops is None:
			self.errors.append("Lorentz's response is null in " + flat_path)
			return False

		# Check if the property is defined in the Lorentz response
		if props[-1] not in Lorentzprops:
			# Only report as error if this property is required
			if required is not None and props[-1] in required:
				self.errors.append("Property '" + flat_path + "' missing in Lorentz's response")
				return False
			# Optional property absent — skip silently
			return True
		Lorentzprop = Lorentzprops[props[-1]]

		# If this is another object, we have to dive deeper
		if YAMLprop['type'] == 'object':
			if 'properties' in YAMLprop:
				nested_required = YAMLprop.get('required', [])
				# Loop over all properties of the object ...
				for prop in YAMLprop['properties']:
					# ... and check them recursively
					if not self.verify_property(YAMLprop['properties'], YAMLexamples, Lorentzprop, props + [prop], nested_required):
						all_okay = False
			elif 'additionalProperties' not in YAMLprop:
				self.errors.append(flat_path + " is an object, but the API specs define it as a simple object")
				return False
		elif YAMLprop['type'] == 'array':
			# Check if the Lorentz response is an array
			if type(Lorentzprop) is not list:
				self.errors.append("Lorentz's response is not an array in " + flat_path)
				return False
			# Loop over all items in the array ...
			for i in range(len(Lorentzprop)):
				# ... and check them recursively if they are objects
				if not type(Lorentzprop[i]) is dict:
					if 'properties' in YAMLprop['items']:
						self.errors.append(flat_path + " is an array, but the API specs define it as an array of objects")
						return False
					else:
						# Simple array and declared as such, no need for further recursion
						continue

				# Check for allOf definitions in an array defining arrays of objects where all components must be checked
				if 'allOf' in YAMLprop['items'] and type(Lorentzprop[i]) is dict:
					for j in Lorentzprop[i]:
						# Collect all allOf components and their required fields
						allOf_props = {}
						allOf_required = []
						for allOf in YAMLprop['items']['allOf']:
							allOf_props.update(allOf['properties'])
							allOf_required.extend(allOf.get('required', []))
						# ... and check them recursively
						if not self.verify_property(allOf_props, YAMLexamples, Lorentzprop[i], props + [i, str(j)], allOf_required):
							all_okay = False
					continue

				if 'properties' not in YAMLprop['items'] and type(Lorentzprop[i]) is dict:
					self.errors.append(flat_path + " is an array of objects, but the API specs define it as a simple array")
					return False

				items_required = YAMLprop['items'].get('required', [])
				for j in Lorentzprop[i]:
					# ... and check them recursively
					if not self.verify_property(YAMLprop['items']['properties'], YAMLexamples, Lorentzprop[i], props + [i, str(j)], items_required):
						all_okay = False

			# Add this property to the YAML response
			self.YAMLresponse[flat_path] = []
		else:
			# Check this property

			# Get type of this property using the YAML_TYPES translation table
			yaml_type = YAMLprop['type']

			# Check if this property is nullable (can be None even
			# if not defined as string, integer, etc.)
			yaml_nullable = 'nullable' in YAMLprop and YAMLprop['nullable'] == True

			# Get format of this property (if defined)
			yaml_format = YAMLprop['format'] if 'format' in YAMLprop else YAMLprop['x-format'] if 'x-format' in YAMLprop else None

			# Add this property to the YAML response
			self.YAMLresponse[flat_path] = []

			# Check type of YAML example (if defined)
			if 'example' in YAMLprop:
				# Check if the type of the example matches the
				# type we defined in the API specs
				self.YAMLresponse[flat_path].append(YAMLprop['example'])
				if not self.verify_type(YAMLprop['example'], yaml_type, yaml_nullable, yaml_format):
					self.errors.append(f"API example ({str(type(YAMLprop['example']))}) does not match defined type ({yaml_type}) in {flat_path} (nullable: " + ("True" if yaml_nullable else "False") + ")")
					return False

			# Check type of externally defined YAML examples (next to schema)
			elif YAMLexamples is not None:
				for t in YAMLexamples:
					if 'value' not in YAMLexamples[t]:
						self.errors.append(f"Example {flat_path} does not have a 'value' property")
						return False
					example = YAMLexamples[t]['value']
					# Dive into the example to get to the property we want
					skip_this = False
					for p in props:
						if type(example) == dict and p not in example:
							self.errors.append(f"Example {t} does not have an '{p}' item")
							return False
						if type(example) == list and p >= len(example):
							# We're out of bounds, so we can't check this example
							skip_this = True
							break
						example = example[p]
					if skip_this:
						continue
					# Check if the type of the example matches the type we defined in the API specs
					self.YAMLresponse[flat_path].append(example)
					if not self.verify_type(example, yaml_type, yaml_nullable, yaml_format):
						self.errors.append(f"API example ({str(type(example))}) does not match defined type ({yaml_type}) in {flat_path} (nullable: " + ("True" if yaml_nullable else "False") + ")")
						return False

			# Compare type of Lorentz's reply against what we defined in the API specs
			if not self.verify_type(Lorentzprop, yaml_type, yaml_nullable, yaml_format):
				self.errors.append(f"Lorentz's reply ({str(type(Lorentzprop))}) does not match defined type ({yaml_type}) in {flat_path}")
				return False
		return all_okay


	def verify_endpoints(self):
		checked_lorentz = 0
		checked_openapi = 0
		# Get Lorentz response
		authentication_method = random.choice([a for a in AuthenticationMethods])
		Lorentzresponse = self.lorentz.GET("/api/endpoints", authenticate = authentication_method)
		if Lorentzresponse is None:
			self.errors.append("No response from Lorentz API")
			return self.errors

		# Construct full URI to check (this is what we specify in OpenAPI specs)
		for method in Lorentzresponse['endpoints']:
			for endpoint in Lorentzresponse['endpoints'][method]:
				endpoint["full_uri"] = endpoint["uri"] + endpoint["parameters"] # type: str
				# If the endpoint starts with /api, remove this part (it is not
				# part of the YAML specs)
				if endpoint["full_uri"].startswith("/api"):
					endpoint["full_uri"] = endpoint["full_uri"][4:]

		# Do the same for the specified endpoints in the OpenAPI specs
		openapi = {}
		for endpoint in self.openapi.paths:
			openapi[endpoint] = {}
			for method in self.openapi.paths[endpoint]:
				if method not in self.openapi.METHODS:
					# Skip keys like "parameters" and "summary"
					continue
				openapi[endpoint][method] = endpoint

		# Check if Lorentz reports endpoints not defined in the API specs
		for method in Lorentzresponse['endpoints']:
			for endpoint in Lorentzresponse['endpoints'][method]:
				m = method.upper() # type: str
				checked_lorentz += 1
				if endpoint["full_uri"] not in self.openapi.paths or method not in self.openapi.paths[endpoint["full_uri"]]:
					self.errors.append("Endpoint " + m + " " + endpoint["full_uri"] + " specified in Lorentz's /api/endpoints not found in OpenAPI specs")

		# Check if all endpoints defined in the API specs are also defined in Lorentz
		for endpoint in openapi:
			for method in openapi[endpoint]:
				full_uris = [ep["full_uri"] for ep in Lorentzresponse['endpoints'][method]] # type: list[str]
				checked_openapi += 1
				if endpoint not in full_uris:
					m = method.upper() # type: str
					self.errors.append("Endpoint " + m + " " + endpoint + " specified in OpenAPI specs not found in Lorentz's /api/endpoints")

		# Check if the number of endpoints checked is the same
		if checked_lorentz != checked_openapi:
			self.errors.append("Number of endpoints checked does not match (Lorentz " + str(checked_lorentz) + " vs. OpenAPI " + str(checked_openapi) + ")")

		checked = max(checked_lorentz, checked_openapi)
		return self.errors, checked
