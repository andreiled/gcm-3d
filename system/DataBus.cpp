#include "DataBus.h"

void error_handler_function(MPI::Comm& comm, int* error_code, ...)
{
	char error_string[MPI::MAX_ERROR_STRING];
	int len;
	MPI::Get_error_string(*error_code, error_string, len);
	
	error_string[len] = 0;
	
	throw new GCMException(GCMException::MPI_EXCEPTION, error_string);
}

DataBus::DataBus()
{
	logger = new Logger();
	DataBus(logger);
};

DataBus::DataBus(Logger* new_logger)
{
	data_bus_type.assign("Basic MPI DataBus");
	attach(new_logger);
	// try to initialize MPI
	// TODO: add command line arguments processing if we really need it
	MPI::Init();
	// register error handler that throws exceptions
	error_handler = MPI::COMM_WORLD.Create_errhandler(&error_handler_function);
	MPI::COMM_WORLD.Set_errhandler(error_handler);
	// MPI initialized, get processor number ant total number of processors
	proc_num = MPI::COMM_WORLD.Get_rank();
	procs_total_num = MPI::COMM_WORLD.Get_size();
	logger->set_proc_num(proc_num);
	*logger < "MPI initialized";
};

DataBus::~DataBus()
{
	// try to finilize MPI
	MPI::Finalize();
	*logger < "MPI finalized";
};

string* DataBus::get_data_bus_type()
{
	return &data_bus_type;
};

void DataBus::attach(Logger* new_logger)
{
	logger = new_logger;
};

void DataBus::attach(TetrMeshSet* new_mesh_set)
{
	mesh_set = new_mesh_set;
};

void DataBus::attach(CollisionDetector* cd)
{
	collision_detector = cd;
};

float DataBus::get_max_possible_tau(float local_time_step)
{
	MPI::COMM_WORLD.Barrier();
	
	float max_tau;     
	MPI::COMM_WORLD.Allreduce(&local_time_step, &max_tau, 1, MPI::FLOAT, MPI::MIN);
	*logger << "Time step synchronized, value is: " < max_tau; 

	return max_tau;
}

int DataBus::sync_nodes()
{
	vector<MPI::Request> reqs;
	MPI::COMM_WORLD.Barrier();
	*logger < "Starting nodes sync";
	for (int i = 0; i < mesh_set->get_number_of_local_meshes(); i++)
	{
		TetrMesh_1stOrder* mesh = mesh_set->get_local_mesh(i);
		for (int j = 0; j < mesh->nodes.size(); j++)
			if (mesh->nodes[j].placement_type == REMOTE)
			{
				int dest = get_proc_for_zone(mesh->nodes[j].remote_zone_num);
				// avoid sending self-requests
				if (dest == proc_num)
				{
					ElasticNode *node= mesh_set->get_mesh_by_zone_num(mesh->nodes[j].remote_zone_num)->get_node(mesh->nodes[j].remote_num);
					memcpy(mesh->nodes[j].values, node->values, 13*sizeof(float));
					memcpy(mesh->nodes[j].coords, node->coords, 3*sizeof(float));
				}
			}
	}
	
	for (int i = 0; i < zones_info.size(); i++)
		if (get_proc_for_zone(i) != proc_num)
			for (int j = 0; j < zones_info.size(); j++)
				if (local_numbers[i][j].size())
				{
					*logger << "Sending nodes from zone " << j << " to zone " < i;
					reqs.push_back(
						MPI::COMM_WORLD.Isend(
							&mesh_set->get_mesh_by_zone_num(j)->nodes[0],
							1,
							MPI_NODE_TYPES[i][j],
							get_proc_for_zone(i),
							TAG_SYNC_NODE+100*i+j
						)
					);
				}
	
	MPI::COMM_WORLD.Barrier();
	
	for (int i = 0; i < zones_info.size(); i++)
		if (get_proc_for_zone(i) == proc_num)
			for (int j = 0; j < zones_info.size(); j++)
				if (local_numbers[i][j].size())
				{
					*logger << "Receiving nodes from zone " << j << " to zone " < i;
					MPI::COMM_WORLD.Recv(
						&mesh_set->get_mesh_by_zone_num(i)->nodes[0],
						1,
						MPI_NODE_TYPES[i][j],
						get_proc_for_zone(j),
						TAG_SYNC_NODE+100*i+j
					);
				}

	MPI::Request::Waitall(reqs.size(), &reqs[0]);
	MPI::COMM_WORLD.Barrier();
	
	*logger < "Nodes sync done";

	// FIXME
	// do we really need to return a value?
	return 0;
};

void DataBus::load_zones_info(vector<int>* map)
{
	zones_info.clear();
	for(int i = 0; i < map->size(); i++)
		zones_info.push_back( map->at(i) );
}

int DataBus::get_proc_num()
{
	return proc_num;
}

int DataBus::get_procs_total_num()
{
	return procs_total_num;
}

int DataBus::get_proc_for_zone(int zone_num)
{
	return zones_info[zone_num];
}

void DataBus::sync_outlines()
{
	MPI::COMM_WORLD.Barrier();
	// calculate displacements
	int *displ = (int*)malloc(sizeof(int)*procs_total_num);
	displ[0] = 0;
	for (int i = 1; i < procs_total_num; i++)
		displ[i] = displ[i-1]+ mesh_set->meshes_at_proc[i-1];
	*logger < "Syncing outlines";
	MPI::COMM_WORLD.Allgatherv(
		mesh_set->get_local_mesh(0),
		mesh_set->meshes_at_proc[proc_num],
		MPI_OUTLINE, mesh_set->get_mesh(0),
		mesh_set->meshes_at_proc,
		displ,
		MPI_OUTLINE
	);
	*logger < "Outlines synced";
	free(displ);
}

void DataBus::terminate()
{
	MPI::COMM_WORLD.Abort(MPI_CODE_TERMINATED);
}

void DataBus::create_custom_types() {
	
	*logger < "Creating custom data types";

	TetrMesh_1stOrder meshes[2];
	Triangle faces[2];
	Tetrahedron_1st_order tetrs[2];
	
	MPI::Datatype outl_types[] = {
		MPI::LB,
		MPI::FLOAT,
		MPI::FLOAT,
		MPI::UB
	};
	
	int outl_lengths[] = {
		1,
		3,
		3,
		1				
	};
	
	MPI::Aint outl_displacements[] = {
		MPI::Get_address(&meshes[0]),
		MPI::Get_address(&meshes[0].outline.min_coords[0]),
		MPI::Get_address(&meshes[0].outline.max_coords[0]),
		MPI::Get_address(&meshes[1])
	};
	
	for (int i = 3; i >=0; i--)
		outl_displacements[i] -= outl_displacements[0];
	
	MPI_OUTLINE = MPI::Datatype::Create_struct(
		4,
		outl_lengths,
		outl_displacements,
		outl_types
	);
	
	MPI::Datatype face_types[] = 
	{
		MPI::LB,
		MPI::INT,
		MPI::INT,
		MPI::UB
	};
	
	int face_lens[] = {
		1,
		1,
		3,
		1
	};
	
	MPI::Aint face_displ[] = {
		MPI::Get_address(&faces[0]),
		MPI::Get_address(&faces[0].local_num),
		MPI::Get_address(&faces[0].vert[0]),
		MPI::Get_address(&faces[1])
	};
	
	for (int i = 3; i >= 0; i--)
		face_displ[i] -= face_displ[0];
	
	MPI_FACE_NUMBERED = MPI::Datatype::Create_struct(
		4,
		face_lens,
		face_displ,
		face_types
	);
	
	MPI::Datatype tetr_types[] = 
	{
		MPI::LB,
		MPI::INT,
		MPI::INT,
		MPI::UB
	};
	
	int tetr_lens[] = {
		1,
		1,
		4,
		1
	};
	
	MPI::Aint tetr_displ[] = {
		MPI::Get_address(&tetrs[0]),
		MPI::Get_address(&tetrs[0].local_num),
		MPI::Get_address(&tetrs[0].vert[0]),
		MPI::Get_address(&tetrs[1])
	};
	
	for (int i = 3; i >= 0; i--)
		tetr_displ[i] -= tetr_displ[0];
	
	MPI_TETR_NUMBERED = MPI::Datatype::Create_struct(
		4,
		tetr_lens,
		tetr_displ,
		tetr_types
	);
	
	// FIXME
	// it's overhead
	local_numbers = new vector<int>*[zones_info.size()];
	vector<int> **remote_numbers = new vector<int>*[zones_info.size()];	
	MPI_NODE_TYPES = new MPI::Datatype*[zones_info.size()];	
	
	for (int i = 0; i < zones_info.size(); i++)
	{
		local_numbers[i] = new vector<int>[zones_info.size()];
		remote_numbers[i] = new vector<int>[zones_info.size()];
		MPI_NODE_TYPES[i] = new MPI::Datatype[zones_info.size()];
	}		
	
	// find all remote nodes
	for (int i = 0; i < mesh_set->get_number_of_local_meshes(); i++)
	{
		TetrMesh_1stOrder *mesh = mesh_set->get_local_mesh(i);
		for (int j = 0; j < mesh->nodes.size(); j++)
			if (mesh->nodes[j].placement_type == REMOTE &&
					get_proc_for_zone(mesh->nodes[j].remote_zone_num) != proc_num)
			{
				int local_zone_num = mesh->nodes[j].local_zone_num;
				int remote_zone_num = mesh->nodes[j].remote_zone_num;
				local_numbers[local_zone_num][remote_zone_num].push_back(mesh->nodes[j].local_num);
				remote_numbers[local_zone_num][remote_zone_num].push_back(mesh->nodes[j].remote_num);
			}
	}
	
	// sync types
	ElasticNode elnodes[2];
	MPI::Datatype elnode_types[] = {
		MPI::LB,
		MPI::FLOAT,
		MPI::FLOAT,
		MPI::UB
	};
	
	int elnode_lens[] = {
		1,
		13,
		3,
		1,
		1,
		1,
		1
	};
	
	MPI::Aint elnode_displs[] = {
		MPI::Get_address(&elnodes[0]),
		MPI::Get_address(&elnodes[0].values[0]),
		MPI::Get_address(&elnodes[0].coords[0]),
		MPI::Get_address(&elnodes[1])
	};
	for (int i = 3; i >= 0; i--)
		elnode_displs[i] -= elnode_displs[0];
	
	MPI_ELNODE = MPI::Datatype::Create_struct(
		4,
		elnode_lens,
		elnode_displs,
		elnode_types
	);
	MPI_ELNODE.Commit();
	
	MPI::Datatype elnoden_types[] = {
		MPI::LB,
		MPI::INT,
		MPI::FLOAT,
		MPI::FLOAT,
		MPI::UB
	};
	
	int elnoden_lens[] = {
		1,
		1,
		13,
		3,
		1
	};
	
	MPI::Aint elnoden_displs[] = {
		MPI::Get_address(&elnodes[0]),
		MPI::Get_address(&elnodes[0].local_num),
		MPI::Get_address(&elnodes[0].values[0]),
		MPI::Get_address(&elnodes[0].coords[0]),
		MPI::Get_address(&elnodes[1])
	};
	for (int i = 4; i >= 0; i--)
		elnoden_displs[i] -= elnoden_displs[0];
	
	MPI_ELNODE_NUMBERED = MPI::Datatype::Create_struct(
		5,
		elnoden_lens,
		elnoden_displs,
		elnoden_types
	);
	
	int max_len = 0;
	for (int i = 0; i < zones_info.size(); i++)
		for (int j = 0; j < zones_info.size(); j++)
			if (local_numbers[i][j].size() > max_len)
				max_len = local_numbers[i][j].size();

	vector<int> lengths;
	for (int i = 0; i < max_len; i++)
		lengths.push_back(1);
	
	int info[3];
	
	vector<MPI::Request> reqs;
	
	for (int i = 0; i < zones_info.size(); i++)
		for (int j = 0; j < zones_info.size(); j++)
			if (local_numbers[i][j].size() > 0)
			{
				info[0] = remote_numbers[i][j].size();
				info[1] = i;
				info[2] = j;
				MPI_NODE_TYPES[i][j] =  MPI_ELNODE.Create_indexed(
					local_numbers[i][j].size(),
					&lengths[0],
					&local_numbers[i][j][0]
				);
				MPI_NODE_TYPES[i][j].Commit();
				reqs.push_back(
					MPI::COMM_WORLD.Isend(
						&remote_numbers[i][j][0],
						remote_numbers[i][j].size(),
						MPI::INT,
						get_proc_for_zone(j),
						TAG_SYNC_NODE_TYPES
					)
				);
				reqs.push_back(
					MPI::COMM_WORLD.Isend(
						info,
						3,
						MPI::INT,
						get_proc_for_zone(j),
						TAG_SYNC_NODE_TYPES_I
					)
				);
			}

	MPI::COMM_WORLD.Barrier();
	
	MPI::Status status;	
	
	while (MPI::COMM_WORLD.Iprobe(MPI::ANY_SOURCE, TAG_SYNC_NODE_TYPES_I, status))
	{
		MPI::COMM_WORLD.Recv(
			info,
			3,
			MPI::INT,
			status.Get_source(),
			TAG_SYNC_NODE_TYPES_I
		);
		local_numbers[info[1]][info[2]].resize(info[0]);
		MPI::COMM_WORLD.Recv(
			&local_numbers[info[1]][info[2]][0],
			info[0],
			MPI::INT,
			status.Get_source(),
			TAG_SYNC_NODE_TYPES
		);
		if (lengths.size() < info[0])
			for (int i = lengths.size(); i < info[0]; i++)
				lengths.push_back(1);
		MPI_NODE_TYPES[info[1]][info[2]] =  MPI_ELNODE.Create_indexed(
			info[0],
			&lengths[0],
			&local_numbers[info[1]][info[2]][0]
		);
		MPI_NODE_TYPES[info[1]][info[2]].Commit();
	}

	MPI::Request::Waitall(reqs.size(), &reqs[0]);
	MPI::COMM_WORLD.Barrier();
	
	for (int i = 0 ; i < zones_info.size(); i++)
		delete[] remote_numbers[i];
	delete[] remote_numbers;
	
	for (int i = 0; i < zones_info.size(); i++)
		for (int j = 0; j < zones_info.size(); j++)
			if (local_numbers[i][j].size())
				if (get_proc_for_zone(i) == proc_num)
					*logger << "Nodes in zone " << i << " from zone " << j << " " <  local_numbers[i][j].size();
				else
					*logger << "Nodes in zone " << j << " for zone " << i << " " <  local_numbers[i][j].size();

	MPI_ELNODE_NUMBERED.Commit();
	MPI_FACE_NUMBERED.Commit();
	MPI_TETR_NUMBERED.Commit();
	MPI_OUTLINE.Commit();
	
	*logger < "Custom data types created";
}

void DataBus::sync_faces_in_intersection(MeshOutline **intersections, int **fs, int **fl)
{
	MPI::COMM_WORLD.Barrier();
	vector<int> tmp;
	int buff[4];
	int procs_to_sync = procs_total_num;
	TetrMesh_1stOrder msh;
	vector<MPI::Request> reqs;
	
	*logger < "Starting faces sync";
	
	*logger < "Sending intersections";
	for (int i = 0; i < mesh_set->get_number_of_local_meshes(); i++)
		for (int j = 0; j < mesh_set->get_number_of_remote_meshes(); j++)
		{
			msh.outline = intersections[i][j];
			if (intersections[i][j].min_coords[0] != intersections[i][j].max_coords[0])
			{
				reqs.push_back(
					MPI::COMM_WORLD.Isend(
						&msh,
						1,
						MPI_OUTLINE,
						get_proc_for_zone(mesh_set->get_remote_mesh(j)->zone_num),
						TAG_SYNC_FACES_REQ_I
					)
				);
				tmp.push_back(mesh_set->get_local_mesh(i)->zone_num);
				tmp.push_back(mesh_set->get_remote_mesh(j)->zone_num);
				reqs.push_back(
					MPI::COMM_WORLD.Isend(
						&tmp[0]+tmp.size()-2,
						2,
						MPI::INT,
						get_proc_for_zone(mesh_set->get_remote_mesh(j)->zone_num),
						TAG_SYNC_FACES_REQ_Z
					)
				);
			}
		}
	tmp.push_back(-1);
	tmp.push_back(-1);
	for (int i = 0; i < procs_total_num; i++)
		reqs.push_back(
			MPI::COMM_WORLD.Isend(
				&tmp[0]+tmp.size()-2,
				2,
				MPI::INT,
				i,
				TAG_SYNC_FACES_REQ_Z
			)
		);
	
	*logger < "All intersections sent";
	MPI::COMM_WORLD.Barrier();
	
	MPI::Status status;
	vector<int> **fidx = new vector<int>*[mesh_set->get_number_of_local_meshes()];
	vector<int> **nidx = new vector<int>*[mesh_set->get_number_of_local_meshes()];
	MPI::Datatype **ft = new MPI::Datatype*[mesh_set->get_number_of_local_meshes()];
	MPI::Datatype **nt = new MPI::Datatype*[mesh_set->get_number_of_local_meshes()];
	
	for (int i = 0; i < mesh_set->get_number_of_local_meshes(); i++)
	{
		fidx[i] = new vector<int>[procs_total_num];
		nidx[i] = new vector<int>[procs_total_num];
		ft[i] = new MPI::Datatype[procs_total_num];
		nt[i] = new MPI::Datatype[procs_total_num];
	}
	
	*logger < "Receiving requests";
	while (procs_to_sync)
	{
		MPI::COMM_WORLD.Recv(buff, 2, MPI::INT, MPI::ANY_SOURCE, TAG_SYNC_FACES_REQ_Z, status);
		if (buff[0] == -1)
		{
			procs_to_sync--;
			*logger << "Proc " << status.Get_source() < " synced";
			continue;
		}
		int source = status.Get_source();
		MPI::COMM_WORLD.Recv(&msh, 1, MPI_OUTLINE, source, TAG_SYNC_FACES_REQ_I);
		TetrMesh_1stOrder *mesh = mesh_set->get_mesh_by_zone_num(buff[1]);
		int idx = mesh-mesh_set->get_local_mesh(0);
		int fsz = fidx[idx][source].size();
		int nsz = nidx[idx][source].size();
		collision_detector->find_faces_in_intersection(
			mesh->border,
			mesh->nodes,
			msh.outline,
			fidx[idx][source]
		);
		for (int i = 0; i < fidx[idx][source].size(); i++)
			for (int j = 0; j < 3; j++ )
			{
				bool found = false;
				for (int k = 0; k < nidx[idx][source].size(); k++)
					if (nidx[idx][source][k] == mesh->border[fidx[idx][source][i]].vert[j])
					{
						found = true;
						break;
					}
				if (!found)
					nidx[idx][source].push_back(mesh->border[fidx[idx][source][i]].vert[j]);
			}
		tmp.push_back(fidx[idx][source].size()-fsz);
		tmp.push_back(nidx[idx][source].size()-nsz);
		tmp.push_back(buff[0]);
		tmp.push_back(buff[1]);
		reqs.push_back(
			MPI::COMM_WORLD.Isend(
				&tmp[0]+tmp.size()-4,
				4,
				MPI::INT,
				source,
				TAG_SYNC_FACES_RESP
			)
		);
	}
	
	*logger < "Requests received, sending responses";
	
	int max_len = 0;
	for (int i = 0; i < mesh_set->get_number_of_local_meshes(); i++)
		for (int j = 0; j < procs_total_num; j++)
		{
			if (nidx[i][j].size() > max_len)
				max_len = nidx[i][j].size();
			if (fidx[i][j].size() > max_len)
				max_len = fidx[i][j].size();
		}
	int *lens = new int[max_len];
	for (int i = 0; i < max_len; i++)
		lens[i] = 1;
	
	for (int i = 0; i < mesh_set->get_number_of_local_meshes(); i++)
		for (int j = 0 ; j < procs_total_num; j++)
			if (nidx[i][j].size())
			{
				nt[i][j] = MPI_ELNODE_NUMBERED.Create_indexed(nidx[i][j].size(), lens, &nidx[i][j][0]);
				ft[i][j] = MPI_FACE_NUMBERED.Create_indexed(fidx[i][j].size(), lens, &fidx[i][j][0]);
				nt[i][j].Commit();
				ft[i][j].Commit();
				*logger << "Sending " << nidx[i][j].size() << " nodes and " << fidx[i][j].size() << " faces to proc " < j;
				reqs.push_back(
					MPI::COMM_WORLD.Isend(
						&mesh_set->get_local_mesh(i)->nodes[0],
						1,
						nt[i][j],
						j,
						TAG_SYNC_FACES_N_RESP
					)
				);
				reqs.push_back(
					MPI::COMM_WORLD.Isend(
						&mesh_set->get_local_mesh(i)->border[0],
						1,
						ft[i][j],
						j,
						TAG_SYNC_FACES_F_RESP
					)
				);
			}
	
	*logger < "Requests processed, getting responses";
	MPI::COMM_WORLD.Barrier();
	
	for (int i = 0; i < mesh_set->get_number_of_remote_meshes(); i++)
		mesh_set->get_remote_mesh(i)->clear_data();
	
	vector<int> fn;
	vector<int> nn;
	
	for (int i = 0; i < zones_info.size(); i++)
	{
		fn.push_back(0);
		nn.push_back(0);
	}
	
	for (int i = 0; i < mesh_set->get_number_of_local_meshes(); i++)
		for (int j = 0; j < mesh_set->get_number_of_remote_meshes(); j++)
			if (intersections[i][j].min_coords[0] != intersections[i][j].max_coords[0])
			{
				TetrMesh_1stOrder *mesh = mesh_set->get_remote_mesh(j);
				int proc = get_proc_for_zone(mesh->zone_num);
				MPI::COMM_WORLD.Recv(
					buff,
					4,
					MPI::INT,
					proc,
					TAG_SYNC_FACES_RESP
				);
				fs[i][j] = fn[mesh->zone_num];
				fl[i][j] = buff[0];
				fn[mesh->zone_num] += buff[0];
				nn[mesh->zone_num] += buff[1];
			}
	for (int i = 0; i < zones_info.size(); i++)
		if (fn[i])
		{
			TetrMesh_1stOrder *mesh = mesh_set->get_mesh_by_zone_num(i);
			mesh->border.resize(fn[i]);
			mesh->nodes.resize(nn[i]);
			MPI::COMM_WORLD.Recv(
				&mesh->border[0],
				fn[i],
				MPI_FACE_NUMBERED,
				get_proc_for_zone(i),
				TAG_SYNC_FACES_F_RESP
			);
			MPI::COMM_WORLD.Recv(
				&mesh->nodes[0],
				nn[i],
				MPI_ELNODE_NUMBERED,
				get_proc_for_zone(i),
				TAG_SYNC_FACES_N_RESP
			);
			*logger << "Got " << nn[i] << " nodes and " << fn[i] << " faces from proc " < get_proc_for_zone(i);
		}
				
	MPI::Request::Waitall(reqs.size(), &reqs[0]);
	MPI::COMM_WORLD.Barrier();
	
	for (int i = 0; i < mesh_set->get_number_of_local_meshes(); i++)
	{
		for (int j = 0; j < procs_total_num; j++)
			if (nidx[i][j].size())
			{
				nt[i][j].Free();
				ft[i][j].Free();
			}
		delete[] fidx[i];
		delete[] nidx[i];		
		delete[] nt[i];
		delete[] ft[i];
	}
	
	delete[] lens;
	delete[] fidx;
	delete[] nidx;
	delete[] nt;
	delete[] ft;
		
	*logger < "Faces sync done";
}

void DataBus::sync_tetrs()
{
	*logger < "Starting tetrs sync";
	MPI::COMM_WORLD.Barrier();
	
	vector<int> *idx = new vector<int>[zones_info.size()];
	vector<int> tmp;
	int procs_to_sync = procs_total_num;
	int buff[3];
	vector<MPI::Request> reqs;
	
	for (int i = 0; i < mesh_set->virt_nodes.size(); i++)
		if (get_proc_for_zone(mesh_set->virt_nodes[i].remote_zone_num) != proc_num)
			idx[mesh_set->virt_nodes[i].remote_zone_num].push_back(mesh_set->virt_nodes[i].remote_num);

	*logger < "Sending requests for tetrs";

	for (int i = 0; i < zones_info.size(); i++)
		if (idx[i].size())
		{
			*logger << "Requesting tetrs for " << idx[i].size() << " nodes from proc " < get_proc_for_zone(i);
			reqs.push_back(
				MPI::COMM_WORLD.Isend(
					&idx[i][0],
					idx[i].size(),
					MPI::INT,
					get_proc_for_zone(i),
					TAG_SYNC_TETRS_REQ
				)
			);
			tmp.push_back(i);
			tmp.push_back(idx[i].size());
			reqs.push_back(
				MPI::COMM_WORLD.Isend(
					&tmp[0]+tmp.size()-2,
					2,
					MPI::INT,
					get_proc_for_zone(i),
					TAG_SYNC_TETRS_REQ_I
				)
			);
		}
	
	for (int i = 0; i < procs_total_num; i++)
	{
		tmp.push_back(-1);
		tmp.push_back(-1);
		reqs.push_back(
			MPI::COMM_WORLD.Isend(
				&tmp[0]+tmp.size()-2,
				2,
				MPI::INT,
				i,
				TAG_SYNC_TETRS_REQ_I
			)
		);		
	}
	
	*logger < "Tetrs requests sent";
	
	MPI::COMM_WORLD.Barrier();
	
	MPI::Status status;
	vector<int> req_idx;
	MPI::Datatype **tt = new MPI::Datatype*[zones_info.size()];
	MPI::Datatype **nt = new MPI::Datatype*[zones_info.size()];
	vector<int> **tidx = new vector<int>*[zones_info.size()];
	vector<int> **nidx = new vector<int>*[zones_info.size()];
	
	for (int i = 0; i < zones_info.size(); i++)
	{
		tt[i] = new MPI::Datatype[procs_total_num];
		nt[i] = new MPI::Datatype[procs_total_num];
		tidx[i] = new vector<int>[procs_total_num];
		nidx[i] = new vector<int>[procs_total_num];
	}
	
	
	*logger < "Receiving requests";
	while (procs_to_sync)
	{
		MPI::COMM_WORLD.Recv(buff, 2, MPI::INT, MPI::ANY_SOURCE, TAG_SYNC_TETRS_REQ_I, status);
		int zn = buff[0];
		if (zn == -1)
		{
			*logger << "Proc " << status.Get_source() < " synced";
			procs_to_sync--;
			continue;
		}
		int source = status.Get_source();
		req_idx.resize(buff[1]);
		MPI::COMM_WORLD.Recv(&req_idx[0], buff[1], MPI::INT, source, TAG_SYNC_TETRS_REQ);
		TetrMesh_1stOrder *mesh = mesh_set->get_mesh_by_zone_num(zn);
		bool found;
		for (int i = 0; i < buff[1]; i++)
		{
			Triangle *face = &mesh->border[0]+req_idx[i];
			for (int j = 0; j < 3; j++)
				for (int k = 0; k < mesh->nodes[face->vert[j]].elements->size(); k++)
				{
					Tetrahedron_1st_order *tetr = &mesh->tetrs[0]+mesh->nodes[face->vert[j]].elements->at(k);
					found = false;
					for (int q = 0; q < tidx[zn][source].size(); q++)
						if (tidx[zn][source][q] == tetr->local_num)
						{
							found = true;
							break;
						}
					if (!found)
						tidx[zn][source].push_back(tetr->local_num);
				}
		}
		for (int i = 0; i < tidx[zn][source].size(); i++)
		{
			Tetrahedron_1st_order *tetr = &mesh->tetrs[0]+tidx[zn][source][i];
			for (int j = 0; j < 4; j++)
			{
				found = false;
				ElasticNode *node = &mesh->nodes[0]+tetr->vert[j];
				for (int k = 0; k < nidx[zn][source].size(); k++)
					if (nidx[zn][source][k] == node->local_num)
					{
						found = true;
						break;
					}
				if (!found)
					nidx[zn][source].push_back(node->local_num);
			}
		}
	}
	
	*logger < "Requests received, sending responses";
	MPI::COMM_WORLD.Barrier();
	
	int max_len = 0;
	for (int i = 0; i < zones_info.size(); i++)
		for (int j = 0; j < procs_total_num; j++)
		{
			if (nidx[i][j].size() > max_len)
				max_len = nidx[i][j].size();
			if (tidx[i][j].size() > max_len)
				max_len = tidx[i][j].size();
		}

	int *lens = new int[max_len];
	for (int i = 0; i < max_len; i++)
		lens[i] = 1;
	
	for (int i = 0; i < zones_info.size(); i++)
		for (int j = 0; j < procs_total_num; j++)
			if (tidx[i][j].size())
			{
				nt[i][j] = MPI_ELNODE_NUMBERED.Create_indexed(nidx[i][j].size(), lens, &nidx[i][j][0]);
				tt[i][j] = MPI_TETR_NUMBERED.Create_indexed(tidx[i][j].size(), lens, &tidx[i][j][0]);
				nt[i][j].Commit();
				tt[i][j].Commit();
				*logger << "Sending " << nidx[i][j].size() << " nodes and " << tidx[i][j].size() << " tetrs to proc " < j;
				reqs.push_back(
					MPI::COMM_WORLD.Isend(
						&mesh_set->get_mesh_by_zone_num(i)->nodes[0],
						1,
						nt[i][j],
						j,
						TAG_SYNC_TETRS_N_RESP
					)
				);
				reqs.push_back(
					MPI::COMM_WORLD.Isend(
						&mesh_set->get_mesh_by_zone_num(i)->tetrs[0],
						1,
						tt[i][j],
						j,
						TAG_SYNC_TETRS_T_RESP
					)
				);
				tmp.push_back(i);
				tmp.push_back(nidx[i][j].size());
				tmp.push_back(tidx[i][j].size());
				reqs.push_back(
					MPI::COMM_WORLD.Isend(
						&tmp[0]+tmp.size()-3,
						3,
						MPI::INT,
						j,
						TAG_SYNC_TETRS_I_RESP
					)
				);
			}
	
	*logger < "Requests processed, getting responses";
	MPI::COMM_WORLD.Barrier();
	
	int cnt = 0;
	
	for (int i = 0; i < zones_info.size(); i++)
		if (idx[i].size())
			cnt++;
	
	for (int i = 0; i < cnt; i++)
	{
		MPI::COMM_WORLD.Recv(buff, 3, MPI::INT, MPI::ANY_SOURCE, TAG_SYNC_TETRS_I_RESP, status);
		TetrMesh_1stOrder *mesh = mesh_set->get_mesh_by_zone_num(buff[0]);
		mesh->clear_data();
		mesh->nodes.resize(buff[1]);
		mesh->tetrs.resize(buff[2]);
		int source = status.Get_source();
		MPI::COMM_WORLD.Recv(
			&mesh->nodes[0],
			buff[1],
			MPI_ELNODE_NUMBERED,
			source,
			TAG_SYNC_TETRS_N_RESP
		);
		MPI::COMM_WORLD.Recv(
			&mesh->tetrs[0],
			buff[2],
			MPI_TETR_NUMBERED,
			source,
			TAG_SYNC_TETRS_T_RESP
		);
		*logger << "Got " << buff[1] << " nodes and " << buff[2] << " tetrs from proc " < source;
	}
	
	MPI::Request::Waitall(reqs.size(), &reqs[0]);	
	MPI::COMM_WORLD.Barrier();


	delete[] idx;
	for (int i = 0; i < zones_info.size(); i++)
	{
		for (int j = 0; j < procs_total_num; j++)
			if (tidx[i][j].size())
			{
				tt[i][j].Free();
				nt[i][j].Free();
			}
		delete[] tt[i];
		delete[] nt[i];
		delete[] tidx[i];
		delete[] nidx[i];
	}
	delete[] lens;
	delete[] tt;
	delete[] nt;
	delete[] tidx;
	delete[] nidx;
	
	*logger < "Tetrs sync done";

}
